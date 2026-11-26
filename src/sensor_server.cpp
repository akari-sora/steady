#include "sensor_server.h"
#include "motion_engine.h"
#include "app_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QSslCertificate>
#include <QSslKey>
#include <QStandardPaths>
#include <algorithm>
#include <memory>
#include <cmath>

namespace {
constexpr int kMaxHttpHeaderBytes = 8 * 1024;
constexpr int kMaxHttpBodyBytes = 16 * 1024;
constexpr quint64 kMaxWsPayloadBytes = 16 * 1024;
constexpr int kMaxClientBufferBytes = 32 * 1024;
constexpr qint64 kMaxClientWriteBytes = 64 * 1024;
constexpr qint64 kMaxUdpDatagramBytes = 4 * 1024;
constexpr int kUdpBatchSize = 32;
constexpr qint64 kNetworkCacheMs = 5000;
constexpr int kMaxConcurrentClients = 16;
constexpr qint64 kHttpIdleTimeoutMs = 30'000;
constexpr qint64 kWsIdleTimeoutMs = 60'000;
constexpr qint64 kSourceReleaseMs = 10'000;
constexpr qint64 kDiscoveryWindowMs = 1000;
constexpr int kMaxDiscoveryRepliesPerWindow = 32;
constexpr int kMaxDiscoveryRequesters = 256;
constexpr qint64 kStaleTlsDirectoryMs = 24 * 60 * 60 * 1000;
constexpr auto kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

bool opensslOverridden()
{
    return qEnvironmentVariableIsSet("STEADY_OPENSSL");
}

QString findOpenSsl()
{
    if (opensslOverridden()) {
        const QString override = qEnvironmentVariable("STEADY_OPENSSL");
        return override.isEmpty() ? QString() : QStandardPaths::findExecutable(override);
    }
#ifdef Q_OS_WIN
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("openssl.exe"));
    if (QFileInfo(bundled).isExecutable()) return bundled;
#endif
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (!onPath.isEmpty()) return onPath;
#ifdef Q_OS_WIN
    return QStandardPaths::findExecutable(QStringLiteral("openssl.exe"), {
        QStringLiteral("C:/Program Files/Git/usr/bin"),
        QStringLiteral("C:/Program Files/OpenSSL-Win64/bin")
    });
#else
    return {};
#endif
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QSslConfiguration tlsConfiguration(const QByteArray &certificate, const QByteArray &privateKey)
{
    const QSslCertificate cert(certificate, QSsl::Pem);
    const QSslKey key(privateKey, QSsl::Rsa, QSsl::Pem);
    if (cert.isNull() || key.isNull() || cert.expiryDate() <= QDateTime::currentDateTimeUtc().addDays(7)) {
        return {};
    }
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificate(cert);
    cfg.setPrivateKey(key);
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    return cfg;
}

bool coversAddresses(const QSslConfiguration &cfg, const QString &lan, const QString &tailscale)
{
    const auto addresses = cfg.localCertificate().subjectAlternativeNames().values(QSsl::IpAddressEntry);
    const auto covers = [&addresses](const QString &address) {
        return address.isEmpty() || addresses.contains(address);
    };
    return covers(QStringLiteral("127.0.0.1")) && covers(lan) && covers(tailscale);
}

qint64 queuedBytes(const QSslSocket *socket)
{
    return socket->bytesToWrite() + socket->encryptedBytesToWrite();
}

void removeStaleTlsDirectories(const QDir &cache)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QFileInfoList entries = cache.entryInfoList({QStringLiteral("tls-*")},
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QFileInfo config(QDir(entry.filePath()).filePath(QStringLiteral("openssl.cnf")));
        const QDateTime modified = config.exists() ? config.lastModified().toUTC()
                                                   : entry.lastModified().toUTC();
        if (modified.isValid() && modified.msecsTo(now) >= kStaleTlsDirectoryMs) {
            QDir(entry.filePath()).removeRecursively();
        }
    }
}
}

SensorServer::SensorServer(MotionEngine *engine, const Options &options, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_options(options)
    , m_httpsPort(options.httpsPort)
    , m_udpPort(options.udpPort)
    , m_httpsStarting(options.httpsEnabled)
    , m_httpsStatus(options.httpsEnabled ? QStringLiteral("Starting HTTPS") : QStringLiteral("HTTPS disabled by option"))
{
    m_uptimeClock.start();
    m_sweepTimer.setInterval(10'000);
    m_sweepTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_sweepTimer, &QTimer::timeout, this, &SensorServer::sweepIdleClients);
    connect(m_engine, &MotionEngine::imuStatusChanged, this, [this]() {
        if (m_engine->phoneConnected() || m_activeSource.isNull()) return;
        m_activeSource.clear();
        if (sourceLockEnabled()) emit lockedSourceChanged();
    });

    startUdp();
    QTimer::singleShot(0, this, [this]() {
        refreshNetwork();
        if (m_httpsStarting) startHttps();
    });
}

SensorServer::~SensorServer()
{
    m_https.close();
    m_sweepTimer.stop();
    for (QSslSocket *socket : m_clients.keys()) {
        socket->disconnect(this);
        socket->abort();
    }
    m_clients.clear();

    if (m_certificateProcess) {
        m_certificateProcess->disconnect(this);
        if (m_certificateProcess->state() != QProcess::NotRunning) {
            m_certificateProcess->kill();
            m_certificateProcess->waitForFinished(500);
        }
    }
}

void SensorServer::setSourceLockEnabled(bool enabled)
{
    if (sourceLockEnabled() == enabled) return;
    m_options.sourceLockEnabled = enabled;
    m_releasedSource.clear();
    emit sourceLockEnabledChanged();
    emit lockedSourceChanged();
}

QString SensorServer::lockedSource() const
{
    return sourceLockEnabled() && !m_activeSource.isNull() ? m_activeSource.toString() : QString();
}

bool SensorServer::releaseSource()
{
    if (!sourceLockEnabled() || m_activeSource.isNull()) return false;
    m_releasedSource = m_activeSource;
    m_releasedUntilMs = m_uptimeClock.elapsed() + kSourceReleaseMs;
    m_engine->disconnectImu();
    return true;
}

bool SensorServer::sourceAllowed(const QHostAddress &address) const
{
    return !sourceLockEnabled() ||
        ((m_activeSource.isNull() || address == m_activeSource) &&
         (address != m_releasedSource || m_uptimeClock.elapsed() >= m_releasedUntilMs));
}

void SensorServer::ingestSample(const QHostAddress &address, double ax, double ay, double az,
                                double gx, double gy, double gz, bool gravityIncluded, const QString &label, bool idle)
{
    if (sourceLockEnabled() && !m_previousSource.isNull() && address != m_previousSource) {
        m_engine->resetImuCalibration();
    }
    if (!m_engine->ingestImu(ax, ay, az, gx, gy, gz, gravityIncluded, label, idle)) return;
    m_releasedSource.clear();
    m_previousSource = address;
    if (address != m_activeSource) {
        m_activeSource = address;
        if (sourceLockEnabled()) emit lockedSourceChanged();
    }
}

QString SensorServer::pairingUrl(const QString &ip, quint16 port)
{
    return ip.isEmpty() ? QString() : QStringLiteral("https://%1:%2").arg(ip).arg(port);
}

QString SensorServer::lanUrl() const
{
    return m_httpsReady ? pairingUrl(m_lanIp, m_httpsPort) : QString();
}

QString SensorServer::tailscaleUrl() const
{
    return m_httpsReady ? pairingUrl(m_tailscaleIp, m_httpsPort) : QString();
}

QString SensorServer::activePairingUrl() const
{
    return m_useTailscaleQr && !tailscaleUrl().isEmpty() ? tailscaleUrl() : lanUrl();
}

QString SensorServer::activeQrUrl() const
{
    if (activePairingUrl().isEmpty()) return {};
    return QStringLiteral("image://pairing/%1").arg(m_qrRevision);
}

QrMatrix SensorServer::activeQr() const
{
    return encodeQr(activePairingUrl());
}

void SensorServer::setUseTailscaleQr(bool v)
{
    if (m_useTailscaleQr == v) return;
    m_useTailscaleQr = v;
    ++m_qrRevision;
    emit endpointsChanged();
}

static SensorServer::Addresses pairingAddresses(const QList<QNetworkInterface> &interfaces)
{
    SensorServer::Addresses addresses;
    for (const auto &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString name = iface.name();
        if (name.startsWith(QLatin1String("docker")) || name.startsWith(QLatin1String("br-"))) {
            continue;
        }
        const bool tailscale = name.contains(QLatin1String("tailscale"), Qt::CaseInsensitive)
            || iface.humanReadableName().contains(QLatin1String("tailscale"), Qt::CaseInsensitive);
        for (const auto &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal()) continue;
            // Tailscale on macOS can use an anonymous utun interface.
            const bool vpnAddress = ip.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10);
            if (tailscale || vpnAddress) {
                if (addresses.tailscale.isEmpty()) addresses.tailscale = ip.toString();
            } else if (addresses.lan.isEmpty()) {
                addresses.lan = ip.toString();
            }
        }
    }
    return addresses;
}

SensorServer::Addresses SensorServer::networkAddresses()
{
    return pairingAddresses(QNetworkInterface::allInterfaces());
}

void SensorServer::refreshNetwork()
{
    const bool initial = !m_networkRefreshClock.isValid();
    m_networkRefreshClock.start();
    m_interfaces = QNetworkInterface::allInterfaces();
    const Addresses addresses = pairingAddresses(m_interfaces);
    if (!initial && m_lanIp == addresses.lan && m_tailscaleIp == addresses.tailscale) return;
    m_lanIp = addresses.lan;
    m_tailscaleIp = addresses.tailscale;
    if (!m_useTailscaleQr && m_lanIp.isEmpty()) m_useTailscaleQr = !m_tailscaleIp.isEmpty();
    ++m_qrRevision;
    emit endpointsChanged();
}

QString SensorServer::discoveryAddress(const QNetworkDatagram &request) const
{
    const QHostAddress destination = request.destinationAddress();
    QHostAddress candidate;
    QHostAddress matchedSubnet;
    for (const auto &interface : m_interfaces) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        const bool incoming = request.interfaceIndex() > 0 && uint(interface.index()) == request.interfaceIndex();
        for (const auto &entry : interface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal()) continue;
            if (ip == destination) return ip.toString();
            if (!incoming) continue;
            if (destination == entry.broadcast() || (entry.prefixLength() >= 0
                    && request.senderAddress().isInSubnet(ip, entry.prefixLength()))) {
                if (matchedSubnet.isNull()) matchedSubnet = ip;
            }
            if (candidate.isNull()) candidate = ip;
        }
    }
    if (!matchedSubnet.isNull()) return matchedSubnet.toString();
    return candidate.isNull() ? m_lanIp : candidate.toString();
}

QString SensorServer::statusMessage() const
{
    const QString udpStatus = udpReady()
        ? QStringLiteral("UDP :%1 ingest and discovery ready").arg(m_udpPort)
        : QStringLiteral("UDP :%1 unavailable: %2").arg(m_udpPort).arg(m_udp.errorString());
    return m_httpsStatus + QStringLiteral(" · ") + udpStatus;
}

void SensorServer::startUdp()
{
    connect(&m_udp, &QUdpSocket::readyRead, this, &SensorServer::handleUdpDatagrams);
    if (m_udp.bind(QHostAddress::AnyIPv4, m_udpPort, QUdpSocket::DontShareAddress)) {
        m_udpPort = m_udp.localPort();
    }
}

void SensorServer::finishHttps(const QString &status, bool ready)
{
    if (!ready) m_https.close();
    m_httpsStarting = false;
    m_httpsReady = ready;
    m_httpsStatus = status;
    ++m_qrRevision;
    emit endpointsChanged();
}

void SensorServer::startHttps()
{
    const QString openssl = findOpenSsl();
    if (openssl.isEmpty()) {
        finishHttps(QStringLiteral("HTTPS off: openssl executable unavailable"));
        return;
    }
    if (!QSslSocket::supportsSsl()) {
        finishHttps(QStringLiteral("HTTPS off: Qt TLS backend unavailable"));
        return;
    }
    if (!m_https.listen(QHostAddress::AnyIPv4, m_httpsPort)) {
        m_httpsBindFailed = true;
        finishHttps(QStringLiteral("HTTPS off: cannot bind :%1 (%2)").arg(m_httpsPort).arg(m_https.errorString()));
        return;
    }
    m_httpsPort = m_https.serverPort();
    m_https.pauseAccepting();

    const QString cache = m_options.cacheDirectory.isEmpty() ? AppPaths::cacheDirectory()
                                                             : m_options.cacheDirectory;
    if (cache.isEmpty() || !QDir().mkpath(cache)) {
        finishHttps(QStringLiteral("HTTPS off: cannot create certificate cache"));
        return;
    }
    const QDir dir(cache);
    removeStaleTlsDirectories(dir);
    const QString pemPath = dir.filePath(QStringLiteral("tls.pem"));
    const QByteArray pem = readFile(pemPath);
    const QByteArray endCertificate("-----END CERTIFICATE-----");
    const qsizetype end = pem.indexOf(endCertificate);
    QSslConfiguration cfg = end < 0
        ? tlsConfiguration(readFile(dir.filePath(QStringLiteral("cert.pem"))),
                           readFile(dir.filePath(QStringLiteral("key.pem"))))
        : tlsConfiguration(pem.first(end + endCertificate.size()),
                           pem.sliced(end + endCertificate.size()).trimmed() + '\n');
    if (!cfg.isNull() && coversAddresses(cfg, m_lanIp, m_tailscaleIp)) {
        listenHttps(cfg);
        return;
    }

    // Generate into a private staging directory, then atomically publish both PEMs together.
    auto staging = std::make_shared<QTemporaryDir>(dir.filePath(QStringLiteral("tls-XXXXXX")));
    if (!staging->isValid()) {
        finishHttps(QStringLiteral("HTTPS off: cannot create certificate staging directory"));
        return;
    }
    const QDir stage(staging->path());
    const QString certPath = stage.filePath(QStringLiteral("cert.pem"));
    const QString keyPath = stage.filePath(QStringLiteral("key.pem"));
    QFile config(stage.filePath(QStringLiteral("openssl.cnf")));
    QByteArray san("IP:127.0.0.1");
    if (!m_lanIp.isEmpty()) san += ",IP:" + m_lanIp.toLatin1();
    if (!m_tailscaleIp.isEmpty()) san += ",IP:" + m_tailscaleIp.toLatin1();
    const QByteArray configText = "[req]\ndistinguished_name=dn\nx509_extensions=extensions\n"
        "prompt=no\n[dn]\nCN=Steady IMU\n[extensions]\nsubjectAltName=" + san + '\n';
    if (!config.open(QIODevice::WriteOnly) || config.write(configText) != configText.size() || !config.flush()) {
        finishHttps(QStringLiteral("HTTPS off: cannot write openssl configuration"));
        return;
    }
    config.close();

    auto *process = new QProcess(this);
    m_certificateProcess = process;
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process]() { process->kill(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        finishHttps(QStringLiteral("HTTPS off: could not start openssl"));
        process->deleteLater();
    });
    connect(process, &QProcess::finished, this,
            [this, process, staging, pemPath, keyPath, certPath](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            finishHttps(QStringLiteral("HTTPS off: openssl certificate generation failed"));
            return;
        }
        const QByteArray certificate = readFile(certPath);
        const QByteArray privateKey = readFile(keyPath);
        const QSslConfiguration cfg = tlsConfiguration(certificate, privateKey);
        if (cfg.isNull()) {
            finishHttps(QStringLiteral("HTTPS off: Qt TLS backend could not load the certificate or key"));
            return;
        }
        QSaveFile file(pemPath);
        if (file.open(QIODevice::WriteOnly) && file.setPermissions(kOwnerOnly)) {
            const QByteArray pem = certificate + privateKey;
            if (file.write(pem) == pem.size()) file.commit();
        }
        listenHttps(cfg);
    });
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    process->start(openssl, {QStringLiteral("req"), QStringLiteral("-x509"),
                             QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
                             QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("365"),
                             QStringLiteral("-config"), config.fileName(),
                             QStringLiteral("-keyout"), keyPath, QStringLiteral("-out"), certPath});
    timeout->start(8000);
}

void SensorServer::listenHttps(const QSslConfiguration &configuration)
{
    m_https.setSslConfiguration(configuration);
    m_https.setHandshakeTimeout(5000);
    connect(&m_https, &QSslServer::pendingConnectionAvailable, this, [this]() {
        while (auto *sock = qobject_cast<QSslSocket *>(m_https.nextPendingConnection())) {
            if (m_clients.size() >= kMaxConcurrentClients) {
                sock->abort();
                sock->deleteLater();
                continue;
            }
            ClientState st;
            sock->setReadBufferSize(kMaxClientBufferBytes);
            st.lastActivityMs = m_uptimeClock.elapsed();
            m_clients.insert(sock, st);
            armHeaderDeadline(sock);
            if (!m_sweepTimer.isActive()) {
                m_sweepTimer.start();
            }

            connect(sock, &QSslSocket::readyRead, this, [this, sock]() {
                handleClientData(sock);
            });
            connect(sock, &QSslSocket::disconnected, this, [this, sock]() {
                removeClient(sock);
            });
            connect(sock, &QAbstractSocket::errorOccurred, this, [this, sock](QAbstractSocket::SocketError) {
                removeClient(sock);
            });
            connect(sock, QOverload<const QList<QSslError> &>::of(&QSslSocket::sslErrors),
                    this, [this, sock](const QList<QSslError> &) {
                removeClient(sock);
            });
            if (sock->bytesAvailable()) handleClientData(sock);
        }
    });

    m_https.resumeAccepting();
#ifdef Q_OS_WIN
    // Some native TLS backends parse PEM keys but only report an import failure at handshake.
    auto *probe = new QSslSocket(this);
    const auto done = [this, probe](bool ready) {
        probe->disconnect(this);
        probe->abort();
        probe->deleteLater();
        if (ready) {
            finishHttps(QStringLiteral("HTTPS :%1 ready (%2)").arg(m_httpsPort).arg(QSslSocket::activeBackend()), true);
        } else {
            m_https.close();
            finishHttps(QStringLiteral("HTTPS off: Qt TLS backend %1 could not complete the local handshake")
                        .arg(QSslSocket::activeBackend()));
        }
    };
    auto *timer = new QTimer(probe);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, probe, [done]() { done(false); });
    connect(probe, &QSslSocket::encrypted, this, [done]() { done(true); });
    connect(probe, &QSslSocket::errorOccurred, this, [done](QAbstractSocket::SocketError) { done(false); });
    probe->setPeerVerifyMode(QSslSocket::VerifyNone);
    probe->connectToHostEncrypted(QStringLiteral("127.0.0.1"), m_httpsPort);
    timer->start(2500);
#else
    finishHttps(QStringLiteral("HTTPS :%1 ready (%2)").arg(m_httpsPort).arg(QSslSocket::activeBackend()), true);
#endif
}

void SensorServer::removeClient(QSslSocket *sock)
{
    if (!m_clients.contains(sock)) return;
    sock->disconnect(this);
    sock->abort();
    m_clients.remove(sock);
    if (m_clients.isEmpty() && m_sweepTimer.isActive()) {
        m_sweepTimer.stop();
    }
    sock->deleteLater();
}

bool SensorServer::queueResponse(QSslSocket *sock, const QByteArray &data)
{
    if (!m_clients.contains(sock)) return false;
    if (data.size() + queuedBytes(sock) > kMaxClientWriteBytes || sock->write(data) != data.size()) {
        removeClient(sock);
        return false;
    }
    return m_clients.contains(sock);
}

void SensorServer::closeClient(QSslSocket *sock, const QByteArray &response)
{
    auto it = m_clients.find(sock);
    if (it == m_clients.end() || it->closing) return;
    it->closing = true;
    if (!response.isEmpty() && !queueResponse(sock, response)) return;
    QTimer::singleShot(2000, sock, [this, sock]() { removeClient(sock); });
    sock->disconnectFromHost();
}

void SensorServer::armHeaderDeadline(QSslSocket *sock)
{
    auto it = m_clients.find(sock);
    if (it == m_clients.end() || it->isWebSocket || it->closing) return;
    const int timeoutMs = std::max(1, m_options.httpHeaderTimeoutMs);
    it->headerDeadlineMs = m_uptimeClock.elapsed() + timeoutMs;
    // A coarse timer may fire early; the deadline check below would then skip the
    // removal and leave the request to the much longer idle sweep.
    QTimer::singleShot(timeoutMs, Qt::PreciseTimer, sock, [this, sock]() {
        const auto current = m_clients.constFind(sock);
        if (current == m_clients.cend() || current->isWebSocket || current->closing) return;
        if (m_uptimeClock.elapsed() >= current->headerDeadlineMs) removeClient(sock);
    });
}

void SensorServer::sweepIdleClients()
{
    const qint64 nowMs = m_uptimeClock.elapsed();
    QList<QSslSocket *> stale;
    for (auto it = m_clients.cbegin(); it != m_clients.cend(); ++it) {
        const qint64 limit = it->isWebSocket ? kWsIdleTimeoutMs : kHttpIdleTimeoutMs;
        if (nowMs - it->lastActivityMs > limit) {
            stale.append(it.key());
        }
    }
    for (QSslSocket *sock : stale) {
        removeClient(sock);
    }
}

void SensorServer::handleUdpDatagrams()
{
    if (m_udpDrainQueued) return;
    for (int i = 0; i < kUdpBatchSize && m_udp.hasPendingDatagrams(); ++i) {
        const qint64 sz = m_udp.pendingDatagramSize();
        if (sz <= 0 || sz > kMaxUdpDatagramBytes) {
            // Drain invalid/oversized datagram safely without allocating unbounded RAM.
            char discard[64];
            m_udp.readDatagram(discard, sizeof(discard));
            continue;
        }
        const auto request = m_udp.receiveDatagram(sz);
        if (!request.isValid()) continue;
        const QByteArray data = request.data();

        const QByteArray trimmed = data.trimmed();
        if (trimmed == "STEADY_DISCOVER" || (trimmed.contains("\"discover\"")
            && QJsonDocument::fromJson(trimmed).object().value(QStringLiteral("cmd")).toString() == QLatin1String("discover"))) {
            // The phone probes with this command to learn that the desktop is still
            // there, so the connected source is always answered. Everyone else is
            // limited per address: a flood from one host must not starve the others.
            const QHostAddress requester = request.senderAddress();
            const bool connectedSource = !m_activeSource.isNull() && requester == m_activeSource;
            if (!connectedSource) {
                const qint64 nowMs = m_uptimeClock.elapsed();
                if (m_discoveryWindows.size() >= kMaxDiscoveryRequesters
                    && !m_discoveryWindows.contains(requester)) {
                    // Drop finished windows first so live counters survive. If every
                    // entry is live, start over: a new phone must still get an answer.
                    m_discoveryWindows.removeIf([nowMs](const auto &entry) {
                        return nowMs - entry.value().startMs >= kDiscoveryWindowMs;
                    });
                    if (m_discoveryWindows.size() >= kMaxDiscoveryRequesters) m_discoveryWindows.clear();
                }
                DiscoveryWindow &window = m_discoveryWindows[requester];
                if (nowMs - window.startMs >= kDiscoveryWindowMs) {
                    window.startMs = nowMs;
                    window.replies = 0;
                }
                if (window.replies >= kMaxDiscoveryRepliesPerWindow) continue;
                ++window.replies;
            }
            if (!m_networkRefreshClock.isValid() || m_networkRefreshClock.elapsed() >= kNetworkCacheMs) {
                refreshNetwork();
            }
            QJsonObject reply;
            reply.insert(QStringLiteral("steady"), true);
            reply.insert(QStringLiteral("lan"), discoveryAddress(request));
            reply.insert(QStringLiteral("ts"), m_tailscaleIp);
            reply.insert(QStringLiteral("udp"), m_udpPort);
            const QByteArray out = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            if (m_udp.writeDatagram(request.makeReply(out)) < 0) {
                m_udp.writeDatagram(out, request.senderAddress(), quint16(request.senderPort()));
            }
            continue;
        }

        parseImuPayload(data, Transport::Udp, request.senderAddress());
    }
    if (m_udp.hasPendingDatagrams()) {
        m_udpDrainQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_udpDrainQueued = false;
            handleUdpDatagrams();
        });
    }
}

void SensorServer::handleClientData(QSslSocket *sock)
{
    auto it = m_clients.find(sock);
    if (it == m_clients.end() || it->closing) return;

    it->lastActivityMs = m_uptimeClock.elapsed();
    const QByteArray chunk = sock->read(kMaxClientBufferBytes - it->buffer.size() + 1);
    it->buffer.append(chunk);

    if (it->buffer.size() > kMaxClientBufferBytes) {
        removeClient(sock);
        return;
    }

    // Loop to handle pipelined HTTP requests or WS frames arriving immediately after upgrade.
    // Re-lookup m_clients.find(sock) every iteration to avoid dangling references if a socket
    // write/flush synchronously triggers disconnected/errorOccurred -> removeClient().
    while (true) {
        auto cur = m_clients.find(sock);
        if (cur == m_clients.end()) return;

        if (cur->isWebSocket) {
            if (!handleWebSocketFrames(sock, *cur)) removeClient(sock);
            return;
        }

        const qsizetype headerEnd = cur->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (cur->buffer.size() > kMaxHttpHeaderBytes) {
                closeClient(sock, "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n");
            }
            return;
        }
        if (headerEnd > kMaxHttpHeaderBytes) {
            closeClient(sock, "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        const QByteArray headerBlock = cur->buffer.left(headerEnd);
        int contentLen = -1;
        const auto lines = headerBlock.split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray &line = lines[i];
            const qsizetype colon = line.indexOf(':');
            const QByteArray name = line.left(colon).trimmed().toLower();
            if (name == "transfer-encoding" || (name == "content-length" && contentLen >= 0)) {
                contentLen = -2;
            } else if (name == "content-length") {
                const QByteArray value = line.mid(colon + 1).trimmed();
                bool ok = false;
                const int parsed = value.toInt(&ok);
                const bool digits = std::all_of(value.cbegin(), value.cend(),
                    [](char c) { return c >= '0' && c <= '9'; });
                contentLen = ok && digits && parsed <= kMaxHttpBodyBytes ? parsed : -2;
            }
            if (contentLen != -2) continue;
            closeClient(sock, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        const qsizetype totalLen = headerEnd + 4 + std::max(0, contentLen);
        if (cur->buffer.size() < totalLen) {
            return;
        }

        const QByteArray req = cur->buffer.left(totalLen);
        cur->buffer.remove(0, totalLen);
        if (!handleHttpRequest(sock, req)) {
            return;
        }
        auto nextIt = m_clients.find(sock);
        if (nextIt != m_clients.end() && !nextIt->isWebSocket) armHeaderDeadline(sock);
        nextIt = m_clients.find(sock);
        if (nextIt == m_clients.end() || nextIt->buffer.isEmpty()) {
            return;
        }
    }
}

bool SensorServer::handleHttpRequest(QSslSocket *sock, const QByteArray &req)
{
    const qsizetype headerEnd = req.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        removeClient(sock);
        return false;
    }
    const QByteArray headerPart = req.left(headerEnd);
    const QByteArray body = req.mid(headerEnd + 4);
    const QList<QByteArray> lines = headerPart.split('\n');
    if (lines.isEmpty()) {
        removeClient(sock);
        return false;
    }

    const QList<QByteArray> first = lines.first().trimmed().split(' ');
    if (first.size() < 2) {
        removeClient(sock);
        return false;
    }
    const QByteArray method = first[0];
    const QByteArray path = first[1];

    // Check for RFC6455 WebSocket upgrade on /ws.
    QByteArray wsKey;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.toLower().startsWith("sec-websocket-key:")) {
            wsKey = line.mid(18).trimmed();
        }
    }

    if (path == "/ws" && !wsKey.isEmpty()) {
        const QByteArray accept = QCryptographicHash::hash(
            wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
            QCryptographicHash::Sha1).toBase64();
        QByteArray resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!queueResponse(sock, resp)) return false;
        auto it = m_clients.find(sock);
        if (it == m_clients.end()) return false;
        it->isWebSocket = true;
        return true;
    }

    if (method == "POST" && path == "/imu") {
        parseImuPayload(body, Transport::Https, sock->peerAddress());
        return queueResponse(sock, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
    }

    if (method == "POST" && path == "/calibrate") {
        if (sourceAllowed(sock->peerAddress())) m_engine->calibrate();
        return queueResponse(sock, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
    }

    if (method == "GET" && path == "/steady-sender.apk") {
        const QStringList candidates = AppPaths::apkCandidates(
            QCoreApplication::applicationDirPath(),
            m_options.dataDirectory.isEmpty() ? AppPaths::dataDirectory() : m_options.dataDirectory);
        auto *file = new QFile(sock);
        for (const QString &p : candidates) {
            file->setFileName(p);
            if (file->open(QIODevice::ReadOnly) && file->size() > 0) break;
            file->close();
        }
        if (!file->isOpen()) {
            const QByteArray msg = "APK not built yet";
            closeClient(sock, "HTTP/1.1 404 Not Found\r\nContent-Length: " + QByteArray::number(msg.size())
                        + "\r\nConnection: close\r\n\r\n" + msg);
            return false;
        }
        QByteArray resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.android.package-archive\r\n"
            "Content-Disposition: attachment; filename=\"steady-sender.apk\"\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Length: " + QByteArray::number(file->size()) + "\r\n"
            "Connection: close\r\n\r\n";
        if (queueResponse(sock, resp)) {
            m_clients[sock].closing = true;
            connect(sock, &QSslSocket::encryptedBytesWritten, this, [this, sock, file]() {
                streamApk(sock, file);
            });
            streamApk(sock, file);
        }
        return false;
    }

    // Default: serve mobile IMU sender page.
    const QByteArray html = buildPhoneHtml();
    QByteArray resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: " + QByteArray::number(html.size()) + "\r\n"
        "Connection: close\r\n\r\n" + html;
    closeClient(sock, resp);
    return false;
}

void SensorServer::streamApk(QSslSocket *sock, QFile *file)
{
    if (!m_clients.contains(sock) || !file->isOpen()) return;
    m_clients[sock].lastActivityMs = m_uptimeClock.elapsed();
    constexpr qint64 chunkSize = 16 * 1024;
    while (queuedBytes(sock) <= kMaxClientWriteBytes / 2) {
        const QByteArray chunk = file->read(chunkSize);
        if (chunk.isEmpty()) {
            file->close();
            // QSslSocket may have handed data to the kernel that the peer has
            // not consumed yet. Let disconnectFromHost() drain it instead of
            // using closeClient()'s short timeout, which truncates slow APK
            // downloads on platforms with smaller TCP receive windows.
            sock->disconnectFromHost();
            QTimer::singleShot(int(kHttpIdleTimeoutMs), this,
                               [this, guarded = QPointer<QSslSocket>(sock)]() {
                if (guarded) removeClient(guarded);
            });
            return;
        }
        if (!queueResponse(sock, chunk)) return;
    }
}

bool SensorServer::handleWebSocketFrames(QSslSocket *sock, ClientState &st)
{
    while (st.buffer.size() >= 2) {
        const quint8 b0 = static_cast<quint8>(st.buffer[0]);
        const quint8 b1 = static_cast<quint8>(st.buffer[1]);
        const quint8 opcode = b0 & 0x0F;
        const bool control = opcode >= 0x8;
        quint64 len = b1 & 0x7F;
        int pos = 2;

        // Only complete, masked client frames, with no negotiated extensions.
        if ((b0 & 0xF0) != 0x80 || !(b1 & 0x80) ||
            (opcode != 0x1 && opcode != 0x2 && opcode != 0x8 && opcode != 0x9 && opcode != 0xA) ||
            (control && len > 125)) {
            return false;
        }
        if (len == 126) {
            if (st.buffer.size() < pos + 2) return true;
            len = (static_cast<quint8>(st.buffer[pos]) << 8) |
                  static_cast<quint8>(st.buffer[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            if (st.buffer.size() < pos + 8) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | static_cast<quint8>(st.buffer[pos + i]);
            }
            pos += 8;
        }

        // Check payload cap BEFORE adding header/mask lengths to prevent 64-bit wraparound & OOM.
        if (len > kMaxWsPayloadBytes) {
            return false;
        }

        const int frameTotal = pos + 4 + int(len);
        if (st.buffer.size() < frameTotal) {
            return true;
        }

        const QByteArray mask = st.buffer.mid(pos, 4);
        QByteArray payload = st.buffer.mid(pos + 4, int(len));
        st.buffer.remove(0, frameTotal);
        for (int i = 0; i < payload.size(); ++i) {
            payload[i] = payload[i] ^ mask[i % 4];
        }

        if (opcode == 0x8) {
            closeClient(sock, QByteArray::fromHex("8800"));
            return true;
        } else if (opcode == 0x9) {
            QByteArray pong;
            pong.append(char(0x8A));
            pong.append(char(payload.size()));
            pong.append(payload);
            if (!queueResponse(sock, pong)) return true;
        } else if (opcode == 0x1 || opcode == 0x2) {
            parseImuPayload(payload, Transport::WebSocket, sock->peerAddress());
            if (!m_clients.contains(sock)) return true;
        }
    }
    return true;
}

void SensorServer::parseImuPayload(const QByteArray &payload, Transport transport, const QHostAddress &address)
{
    if (!sourceAllowed(address)) return;
    const QByteArray trimmed = payload.trimmed();
    if (trimmed.isEmpty()) return;

    if (trimmed.startsWith('{')) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
        const QJsonObject obj = doc.object();

        if (obj.value(QStringLiteral("cmd")).toString() == QLatin1String("calibrate")) {
            m_engine->calibrate();
            return;
        }

        // Direct JSON format: {"ax":..., "ay":..., "az":..., "gz":..., "grav":true}
        if (obj.value(QStringLiteral("ax")).isDouble() || obj.value(QStringLiteral("x")).isDouble()) {
            const double ax = obj.contains(QStringLiteral("ax"))
                ? obj.value(QStringLiteral("ax")).toDouble()
                : obj.value(QStringLiteral("x")).toDouble();
            const double ay = obj.contains(QStringLiteral("ay"))
                ? obj.value(QStringLiteral("ay")).toDouble()
                : obj.value(QStringLiteral("y")).toDouble();
            const double az = obj.contains(QStringLiteral("az"))
                ? obj.value(QStringLiteral("az")).toDouble()
                : obj.value(QStringLiteral("z")).toDouble();
            const double gx = obj.value(QStringLiteral("gx")).toDouble();
            const double gy = obj.value(QStringLiteral("gy")).toDouble();
            const double gz = obj.value(QStringLiteral("gz")).toDouble();
            const bool grav = obj.value(QStringLiteral("grav")).toBool(true);
            QString label = obj.value(QStringLiteral("src")).toString();
            constexpr qsizetype maxLabelLength = 32;
            if (label.size() > maxLabelLength) {
                label.truncate(maxLabelLength);
                if (label.back().isHighSurrogate()) label.chop(1);
            }
            for (auto &ch : label) {
                const auto category = ch.category();
                if (category == QChar::Other_Control || category == QChar::Other_Format ||
                    category == QChar::Separator_Line || category == QChar::Separator_Paragraph) {
                    ch = QLatin1Char(' ');
                }
            }
            if (label.isEmpty()) {
                switch (transport) {
                case Transport::Udp: label = QStringLiteral("UDP 感應器"); break;
                case Transport::Https: label = QStringLiteral("手機網頁 (HTTPS)"); break;
                case Transport::WebSocket: label = QStringLiteral("手機網頁 (WSS)"); break;
                }
            }
            ingestSample(address, ax, ay, az, gx, gy, gz, grav, label, obj.value(QStringLiteral("idle")).toBool());
            return;
        }

        // Sensor Logger app format: {"payload": [{"name":"accelerometer","values":{"x":..,"y":..,"z":..}}]}
        if (obj.contains(QStringLiteral("payload")) && obj.value(QStringLiteral("payload")).isArray()) {
            double ax = 0, ay = 0, az = 0, gz = 0;
            bool gotAccel = false;
            bool grav = true;
            for (const QJsonValue &v : obj.value(QStringLiteral("payload")).toArray()) {
                const QJsonObject item = v.toObject();
                const QString name = item.value(QStringLiteral("name")).toString().toLower();
                const QJsonObject vals = item.value(QStringLiteral("values")).toObject();
                if (vals.value(QStringLiteral("x")).isDouble()
                    && (name == QLatin1String("linearacceleration") || name == QLatin1String("accelerometeruncalibrated") || name == QLatin1String("accelerometer"))) {
                    ax = vals.value(QStringLiteral("x")).toDouble();
                    ay = vals.value(QStringLiteral("y")).toDouble();
                    az = vals.value(QStringLiteral("z")).toDouble();
                    grav = (name != QLatin1String("linearacceleration"));
                    gotAccel = true;
                } else if (name.startsWith(QLatin1String("gyroscope"))) {
                    gz = vals.value(QStringLiteral("z")).toDouble();
                }
            }
            if (gotAccel) {
                ingestSample(address, ax, ay, az, 0, 0, gz, grav, QStringLiteral("Sensor Logger"));
            }
            return;
        }
        return;
    }

    // HyperIMU CSV: optional leading timestamp and optional trailing gyro triple.
    const QList<QByteArray> parts = trimmed.split(',');
    const bool hasTimestamp = parts.size() == 4 || parts.size() == 7;
    const bool hasGyro = parts.size() == 6 || parts.size() == 7;
    if (parts.size() == 3 || hasTimestamp || hasGyro) {
        const int off = hasTimestamp ? 1 : 0;
        bool ok1 = false, ok2 = false, ok3 = false;
        const double ax = parts[off].trimmed().toDouble(&ok1);
        const double ay = parts[off + 1].trimmed().toDouble(&ok2);
        const double az = parts[off + 2].trimmed().toDouble(&ok3);
        if (ok1 && ok2 && ok3 && std::isfinite(ax) && std::isfinite(ay) && std::isfinite(az)) {
            bool okGz = false;
            const double gz = hasGyro ? parts[off + 5].trimmed().toDouble(&okGz) : 0.0;
            ingestSample(address, ax, ay, az, 0, 0, (okGz && std::isfinite(gz)) ? gz : 0.0, true, QStringLiteral("UDP CSV"));
        }
    }
}

QByteArray SensorServer::buildPhoneHtml() const
{
    QString html = QString::fromUtf8(R"HTML(<!doctype html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Steady 手機 IMU</title>
<style>
  body { margin:0; background:#0c0b0c; color:#fafcfb; font-family:system-ui,-apple-system,sans-serif;
         display:flex; flex-direction:column; align-items:center; justify-content:center;
         min-height:100vh; padding:24px; box-sizing:border-box; text-align:center; }
  h1 { margin:0 0 8px; font-size:24px; letter-spacing:0.5px; }
  p { margin:0 0 20px; color:#a8a0a3; font-size:14px; max-width:340px; line-height:1.5; }
  .card { background:#181618; border:1px solid #2e2a2d; border-radius:16px; padding:20px; width:100%; max-width:340px; }
  button { width:100%; padding:16px; border:none; border-radius:12px; font-size:17px; font-weight:600;
           cursor:pointer; margin-bottom:12px; transition:opacity .15s; }
  #toggle { background:#b59790; color:#0c0b0c; }
  #toggle.active { background:#e05a5a; color:#fff; }
  #cal { background:#2a2629; color:#fafcfb; }
  .stat { font-family:ui-monospace,monospace; font-size:13px; color:#cfd3cd; margin-top:8px; }
  .badge { display:inline-block; padding:4px 10px; border-radius:999px; font-size:12px;
           background:#262225; color:#b59790; margin-bottom:16px; }
</style>
</head>
<body>
  <div class="badge" id="status">待機中</div>
  <h1>Steady 手機感應器</h1>
  <p>想要<b>縮小 App 或關螢幕</b>省電持續傳送，請安裝下方 Android 背景版 APK（12.5 Hz 活躍／靜止自動降 1 Hz）。</p>
  <div class="card" style="margin-bottom:14px; border-color:#524441;">
    <a href="/steady-sender.apk" style="display:block; text-decoration:none; background:#b59790; color:#0c0b0c; padding:14px; border-radius:12px; font-size:16px; font-weight:700; margin-bottom:10px;">下載 Android 背景版 APK（省電推薦）</a>
    <a id="deeplink" href="#" style="display:block; text-decoration:none; background:#2a2629; color:#fafcfb; padding:11px; border-radius:10px; font-size:13px; font-weight:600;">已安裝？點此自動帶入目前 IP 開啟 App</a>
    <div class="stat" id="udpinfo" style="margin-top:8px; color:#a8a0a3;"></div>
  </div>
  <div class="card">
    <div style="font-size:12px; color:#a8a0a3; margin-bottom:10px;">網頁快速測試（需保持螢幕開啟）</div>
    <button id="toggle">開始網頁串流 IMU</button>
    <button id="cal">一鍵校正原點</button>
    <div class="stat" id="readout">ax: 0.00 | ay: 0.00 | gz: 0.00</div>
    <div class="stat" id="hz">0 Hz</div>
  </div>
<script>
const UDP_PORT = __UDP_PORT__;
const LAN_IP = "__LAN_IP__";
const TS_IP = "__TS_IP__";
const hostIp = location.hostname;
const qs = new URLSearchParams({ host: hostIp, port: String(UDP_PORT) });
if (LAN_IP) qs.set('lan', LAN_IP);
if (TS_IP) qs.set('ts', TS_IP);
document.getElementById('deeplink').href = `steady://connect?${qs.toString()}`;
document.getElementById('udpinfo').textContent = `UDP 位址：${hostIp}:${UDP_PORT}`;

let running = false, ws = null, wakeLock = null, sent = 0, lastTick = performance.now(), retryMs = 1000, retryTimer = null;
const statusEl = document.getElementById('status');
const toggleBtn = document.getElementById('toggle');
const calBtn = document.getElementById('cal');
const readoutEl = document.getElementById('readout');
const hzEl = document.getElementById('hz');

function connectWs() {
  if (!running) return;
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws');
  ws.onopen = () => { retryMs = 1000; statusEl.textContent = '已連線 (WSS 串流中)'; };
  ws.onclose = () => {
    if (running) {
      statusEl.textContent = `重連中 (${Math.round(retryMs/1000)}s)...`;
      retryTimer = setTimeout(connectWs, retryMs);
      retryMs = Math.min(retryMs * 2, 10000);
    }
  };
}

let lastSend = 0;
function onMotion(e) {
  if (!running) return;
  const now = performance.now();
  if (now - lastSend < 38) return; // ~25 Hz cap
  lastSend = now;

  const lin = e.acceleration;
  const gravAcc = e.accelerationIncludingGravity;
  const useLin = lin && (lin.x !== null || lin.y !== null);
  const a = useLin ? lin : gravAcc;
  if (!a) return;

  const rot = e.rotationRate || {};
  const ax = +(a.x || 0).toFixed(3);
  const ay = +(a.y || 0).toFixed(3);
  const az = +(a.z || 0).toFixed(3);
  const gz = +(-((rot.alpha || 0) * Math.PI) / 180).toFixed(3);

  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify({ ax, ay, az, gz, grav: !useLin }));
    sent++;
  }
  if (now - lastTick >= 500) {
    const hz = Math.round(sent / ((now - lastTick) / 1000));
    sent = 0; lastTick = now;
    readoutEl.textContent = `ax: ${ax.toFixed(2)} | ay: ${ay.toFixed(2)} | gz: ${gz.toFixed(2)}`;
    hzEl.textContent = hz + ' Hz';
  }
}

toggleBtn.onclick = async () => {
  if (!running) {
    if (typeof DeviceMotionEvent !== 'undefined' && typeof DeviceMotionEvent.requestPermission === 'function') {
      const res = await DeviceMotionEvent.requestPermission();
      if (res !== 'granted') { statusEl.textContent = '未授予感應器權限'; return; }
    }
    running = true;
    retryMs = 1000;
    toggleBtn.textContent = '停止串流';
    toggleBtn.classList.add('active');
    connectWs();
    window.addEventListener('devicemotion', onMotion, true);
    try { if ('wakeLock' in navigator) wakeLock = await navigator.wakeLock.request('screen'); } catch {}
  } else {
    running = false;
    if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
    toggleBtn.textContent = '開始網頁串流 IMU';
    toggleBtn.classList.remove('active');
    statusEl.textContent = '已停止';
    window.removeEventListener('devicemotion', onMotion, true);
    if (ws) { ws.close(); ws = null; }
    if (wakeLock) { wakeLock.release(); wakeLock = null; }
  }
};

calBtn.onclick = () => {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify({ cmd: 'calibrate' }));
  } else {
    fetch('/calibrate', { method: 'POST' }).catch(() => {});
  }
  statusEl.textContent = '已送出原點校正';
};
</script>
</body>
</html>)HTML");

    html.replace(QStringLiteral("__UDP_PORT__"), QString::number(m_udpPort));
    html.replace(QStringLiteral("__LAN_IP__"), m_lanIp);
    html.replace(QStringLiteral("__TS_IP__"), m_tailscaleIp);
    return html.toUtf8();
}
