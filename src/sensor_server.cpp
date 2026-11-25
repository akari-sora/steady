#include "sensor_server.h"
#include "motion_engine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QSslCertificate>
#include <QSslKey>
#include <QStandardPaths>
#include <QUrl>
#include <cmath>

namespace {
constexpr int kMaxHttpHeaderBytes = 8 * 1024;
constexpr int kMaxHttpBodyBytes = 16 * 1024;
constexpr quint64 kMaxWsPayloadBytes = 16 * 1024;
constexpr int kMaxClientBufferBytes = 32 * 1024;
constexpr qint64 kMaxUdpDatagramBytes = 4 * 1024;
constexpr int kMaxConcurrentClients = 16;
constexpr qint64 kHttpIdleTimeoutMs = 30'000;
constexpr qint64 kWsIdleTimeoutMs = 60'000;
}

SensorServer::SensorServer(MotionEngine *engine, int httpsPort, int udpPort, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_httpsPort(httpsPort)
    , m_udpPort(udpPort)
{
    m_uptimeClock.start();
    m_sweepTimer.setInterval(10'000);
    m_sweepTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_sweepTimer, &QTimer::timeout, this, &SensorServer::sweepIdleClients);

    refreshNetwork();
    startUdp();
    startHttps();
}

QString SensorServer::lanUrl() const
{
    if (m_lanIp.isEmpty()) return QString();
    return QStringLiteral("https://%1:%2").arg(m_lanIp).arg(m_httpsPort);
}

QString SensorServer::tailscaleUrl() const
{
    if (m_tailscaleIp.isEmpty()) return QString();
    return QStringLiteral("https://%1:%2").arg(m_tailscaleIp).arg(m_httpsPort);
}

QString SensorServer::activeQrPath() const
{
    const QString path = (m_useTailscaleQr && !m_tsQrPath.isEmpty()) ? m_tsQrPath : m_lanQrPath;
    if (path.isEmpty() || !QFile::exists(path)) return QString();
    return QUrl::fromLocalFile(path).toString();
}

void SensorServer::setUseTailscaleQr(bool v)
{
    if (m_useTailscaleQr == v) return;
    m_useTailscaleQr = v;
    emit endpointsChanged();
}

void SensorServer::refreshNetwork()
{
    m_lanIp.clear();
    m_tailscaleIp.clear();

    const auto ifaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : ifaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const QString name = iface.name();
        if (name.startsWith(QLatin1String("docker")) || name.startsWith(QLatin1String("br-"))) {
            continue;
        }
        for (const auto &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) continue;
            const QString s = ip.toString();
            if (name.startsWith(QLatin1String("tailscale")) || s.startsWith(QLatin1String("100."))) {
                if (m_tailscaleIp.isEmpty()) m_tailscaleIp = s;
            } else if (m_lanIp.isEmpty()) {
                m_lanIp = s;
            }
        }
    }
    regenerateQrCodesAsync();
    emit endpointsChanged();
}

void SensorServer::regenerateQrCodesAsync()
{
    const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/steady");
    QDir().mkpath(cacheDir);

    if (!lanUrl().isEmpty()) {
        spawnQrJob(lanUrl(), cacheDir + QStringLiteral("/qr_lan.png"), false);
    } else {
        m_lanQrPath.clear();
    }
    if (!tailscaleUrl().isEmpty()) {
        spawnQrJob(tailscaleUrl(), cacheDir + QStringLiteral("/qr_ts.png"), true);
    } else {
        m_tsQrPath.clear();
    }
}

void SensorServer::spawnQrJob(const QString &url, const QString &outPath, bool isTailscale)
{
    auto *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, outPath, isTailscale](int exitCode, QProcess::ExitStatus status) {
                proc->deleteLater();
                if (status == QProcess::NormalExit && exitCode == 0 && QFile::exists(outPath)) {
                    if (isTailscale) {
                        m_tsQrPath = outPath;
                    } else {
                        m_lanQrPath = outPath;
                    }
                    m_qrRevision++;
                    emit endpointsChanged();
                }
            });
    connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
    proc->start(QStringLiteral("qrencode"),
                {QStringLiteral("-s"), QStringLiteral("7"),
                 QStringLiteral("-m"), QStringLiteral("2"),
                 QStringLiteral("-o"), outPath, url});
}

void SensorServer::startUdp()
{
    connect(&m_udp, &QUdpSocket::readyRead, this, &SensorServer::handleUdpDatagrams);
    if (m_udp.bind(QHostAddress::AnyIPv4, quint16(m_udpPort),
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        m_udpPort = m_udp.localPort();
    }
}

bool SensorServer::ensureSelfSignedCert(const QString &certPath, const QString &keyPath)
{
    if (QFile::exists(certPath) && QFile::exists(keyPath)) {
        QFile cf(certPath), kf(keyPath);
        if (cf.open(QIODevice::ReadOnly) && kf.open(QIODevice::ReadOnly)) {
            const QSslCertificate existing(&cf, QSsl::Pem);
            const QSslKey existingKey(&kf, QSsl::Rsa, QSsl::Pem);
            if (!existing.isNull() && !existingKey.isNull() &&
                existing.expiryDate() > QDateTime::currentDateTimeUtc().addDays(7)) {
                QFile::setPermissions(keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                return true;
            }
        }
    }

    QString san = QStringLiteral("IP:127.0.0.1");
    if (!m_lanIp.isEmpty()) san += QStringLiteral(",IP:") + m_lanIp;
    if (!m_tailscaleIp.isEmpty()) san += QStringLiteral(",IP:") + m_tailscaleIp;

    QProcess proc;
    proc.start(QStringLiteral("openssl"), {
        QStringLiteral("req"), QStringLiteral("-x509"),
        QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
        QStringLiteral("-nodes"),
        QStringLiteral("-keyout"), keyPath,
        QStringLiteral("-out"), certPath,
        QStringLiteral("-days"), QStringLiteral("365"),
        QStringLiteral("-subj"), QStringLiteral("/CN=Steady Omarchy IMU"),
        QStringLiteral("-addext"), QStringLiteral("subjectAltName=") + san
    });
    const bool ok = proc.waitForFinished(8000) && proc.exitCode() == 0;
    if (ok) {
        QFile::setPermissions(keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return ok;
}

void SensorServer::startHttps()
{
    const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/steady");
    QDir().mkpath(cacheDir);
    const QString certPath = cacheDir + QStringLiteral("/cert.pem");
    const QString keyPath = cacheDir + QStringLiteral("/key.pem");

    if (!ensureSelfSignedCert(certPath, keyPath)) {
        m_statusMessage = QStringLiteral("無法產生自簽憑證（仍可使用 UDP %1）").arg(m_udpPort);
        emit endpointsChanged();
        return;
    }

    QFile cf(certPath), kf(keyPath);
    if (!cf.open(QIODevice::ReadOnly) || !kf.open(QIODevice::ReadOnly)) {
        m_statusMessage = QStringLiteral("讀取憑證失敗");
        emit endpointsChanged();
        return;
    }

    QSslCertificate cert(&cf, QSsl::Pem);
    QSslKey key(&kf, QSsl::Rsa, QSsl::Pem);
    QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
    sslCfg.setLocalCertificate(cert);
    sslCfg.setPrivateKey(key);
    sslCfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    m_https.setSslConfiguration(sslCfg);

    connect(&m_https, &QSslServer::pendingConnectionAvailable, this, [this]() {
        while (auto *sock = qobject_cast<QSslSocket *>(m_https.nextPendingConnection())) {
            if (m_clients.size() >= kMaxConcurrentClients) {
                sock->abort();
                sock->deleteLater();
                continue;
            }
            ClientState st;
            st.lastActivityMs = m_uptimeClock.elapsed();
            m_clients.insert(sock, st);
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
        }
    });

    if (m_https.listen(QHostAddress::AnyIPv4, quint16(m_httpsPort))) {
        m_httpsPort = m_https.serverPort();
        m_httpsReady = true;
        m_statusMessage = QStringLiteral("HTTPS :%1 + UDP :%2 就緒").arg(m_httpsPort).arg(m_udpPort);
    } else {
        m_statusMessage = QStringLiteral("HTTPS 監聽失敗: %1").arg(m_https.errorString());
    }
    emit endpointsChanged();
}

void SensorServer::removeClient(QSslSocket *sock)
{
    const bool wasTracked = m_clients.remove(sock) > 0;
    if (m_clients.isEmpty() && m_sweepTimer.isActive()) {
        m_sweepTimer.stop();
    }
    if (!wasTracked) return;

    if (sock->state() == QAbstractSocket::UnconnectedState) {
        sock->deleteLater();
    } else {
        connect(sock, &QAbstractSocket::disconnected, sock, &QObject::deleteLater);
        sock->disconnectFromHost();
        QTimer::singleShot(2000, sock, &QObject::deleteLater);
    }
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
        sock->abort();
        removeClient(sock);
    }
}

void SensorServer::handleUdpDatagrams()
{
    while (m_udp.hasPendingDatagrams()) {
        const qint64 sz = m_udp.pendingDatagramSize();
        if (sz <= 0 || sz > kMaxUdpDatagramBytes) {
            // Drain invalid/oversized datagram safely without allocating unbounded RAM.
            char discard[64];
            m_udp.readDatagram(discard, sizeof(discard));
            continue;
        }
        QByteArray data;
        data.resize(int(sz));
        QHostAddress senderAddr;
        quint16 senderPort = 0;
        const qint64 n = m_udp.readDatagram(data.data(), data.size(), &senderAddr, &senderPort);
        if (n <= 0) continue;
        data.truncate(int(n));

        const QByteArray trimmed = data.trimmed();
        if (trimmed == "STEADY_DISCOVER" || trimmed.contains("\"discover\"")) {
            QJsonObject reply;
            reply.insert(QStringLiteral("steady"), true);
            reply.insert(QStringLiteral("lan"), m_lanIp);
            reply.insert(QStringLiteral("ts"), m_tailscaleIp);
            reply.insert(QStringLiteral("udp"), m_udpPort);
            const QByteArray out = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            m_udp.writeDatagram(out, senderAddr, senderPort);
            continue;
        }

        parseImuPayload(data, QStringLiteral("UDP 感應器"));
    }
}

void SensorServer::handleClientData(QSslSocket *sock)
{
    auto it = m_clients.find(sock);
    if (it == m_clients.end()) return;

    it->lastActivityMs = m_uptimeClock.elapsed();
    const QByteArray chunk = sock->read(kMaxClientBufferBytes - it->buffer.size() + 1);
    it->buffer.append(chunk);

    if (it->buffer.size() > kMaxClientBufferBytes) {
        sock->abort();
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
            if (!handleWebSocketFrames(sock, *cur)) {
                if (m_clients.contains(sock)) {
                    sock->abort();
                    removeClient(sock);
                }
            }
            return;
        }

        const int headerEnd = cur->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (cur->buffer.size() > kMaxHttpHeaderBytes) {
                sock->write("HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n");
                removeClient(sock);
            }
            return;
        }
        if (headerEnd > kMaxHttpHeaderBytes) {
            sock->write("HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n");
            removeClient(sock);
            return;
        }

        const QByteArray headerBlock = cur->buffer.left(headerEnd);
        int contentLen = 0;
        for (const QByteArray &line : headerBlock.split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                bool ok = false;
                const qint64 parsed = line.mid(15).trimmed().toLongLong(&ok);
                if (!ok || parsed < 0 || parsed > kMaxHttpBodyBytes) {
                    sock->write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
                    removeClient(sock);
                    return;
                }
                contentLen = int(parsed);
            }
        }

        const int totalLen = headerEnd + 4 + contentLen;
        if (cur->buffer.size() < totalLen) {
            return;
        }

        const QByteArray req = cur->buffer.left(totalLen);
        cur->buffer.remove(0, totalLen);
        if (!handleHttpRequest(sock, req)) {
            return;
        }
        auto nextIt = m_clients.find(sock);
        if (nextIt == m_clients.end() || nextIt->buffer.isEmpty()) {
            return;
        }
    }
}

bool SensorServer::handleHttpRequest(QSslSocket *sock, const QByteArray &req)
{
    const int headerEnd = req.indexOf("\r\n\r\n");
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
        sock->write(resp);
        auto it = m_clients.find(sock);
        if (it == m_clients.end()) return false;
        it->isWebSocket = true;
        return true;
    }

    if (method == "POST" && path == "/imu") {
        parseImuPayload(body, QStringLiteral("手機網頁 (HTTPS)"));
        sock->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
        return m_clients.contains(sock);
    }

    if (method == "POST" && path == "/calibrate") {
        m_engine->calibrate();
        sock->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
        return m_clients.contains(sock);
    }

    if (method == "GET" && path == "/steady-sender.apk") {
        const QStringList candidates = {
            QCoreApplication::applicationDirPath() + QStringLiteral("/steady-sender.apk"),
            QStringLiteral("/usr/share/steady/steady-sender.apk"),
            QDir::homePath() + QStringLiteral("/Projects/steady/build/steady-sender.apk"),
            QDir::homePath() + QStringLiteral("/.local/share/steady/steady-sender.apk")
        };
        QByteArray apkBytes;
        for (const QString &p : candidates) {
            QFile f(p);
            if (f.exists() && f.open(QIODevice::ReadOnly)) {
                apkBytes = f.readAll();
                break;
            }
        }
        if (apkBytes.isEmpty()) {
            const QByteArray msg = "APK not built yet";
            sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: " + QByteArray::number(msg.size()) + "\r\nConnection: close\r\n\r\n" + msg);
            sock->flush();
            sock->disconnectFromHost();
            return false;
        }
        QByteArray resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.android.package-archive\r\n"
            "Content-Disposition: attachment; filename=\"steady-sender.apk\"\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Length: " + QByteArray::number(apkBytes.size()) + "\r\n"
            "Connection: close\r\n\r\n" + apkBytes;
        sock->write(resp);
        sock->flush();
        sock->disconnectFromHost();
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
    sock->write(resp);
    sock->flush();
    sock->disconnectFromHost();
    return false;
}

bool SensorServer::handleWebSocketFrames(QSslSocket *sock, ClientState &st)
{
    while (st.buffer.size() >= 2) {
        const quint8 b0 = static_cast<quint8>(st.buffer[0]);
        const quint8 b1 = static_cast<quint8>(st.buffer[1]);
        const quint8 opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        quint64 len = b1 & 0x7F;
        int pos = 2;

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

        const int maskLen = masked ? 4 : 0;
        const int frameTotal = pos + maskLen + int(len);
        if (st.buffer.size() < frameTotal) {
            return true;
        }

        QByteArray mask;
        if (masked) {
            mask = st.buffer.mid(pos, 4);
            pos += 4;
        }

        QByteArray payload = st.buffer.mid(pos, int(len));
        st.buffer.remove(0, frameTotal);

        if (masked) {
            for (int i = 0; i < payload.size(); ++i) {
                payload[i] = payload[i] ^ mask[i % 4];
            }
        }

        if (opcode == 0x8) {
            sock->disconnectFromHost();
            return true;
        } else if (opcode == 0x9) {
            QByteArray pong;
            pong.append(char(0x8A));
            pong.append(char(0x00));
            sock->write(pong);
            if (!m_clients.contains(sock)) return true;
        } else if (opcode == 0x1 || opcode == 0x2) {
            parseImuPayload(payload, QStringLiteral("手機網頁 (WSS)"));
            if (!m_clients.contains(sock)) return true;
        }
    }
    return true;
}

void SensorServer::parseImuPayload(const QByteArray &payload, const QString &source)
{
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
        if (obj.contains(QStringLiteral("ax")) || obj.contains(QStringLiteral("x"))) {
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
            const QString customSrc = obj.value(QStringLiteral("src")).toString().left(32);
            QString label = source;
            if (!customSrc.isEmpty()) {
                label = source.contains(QLatin1String("UDP"))
                    ? (customSrc + QStringLiteral(" (UDP)"))
                    : customSrc;
            }
            m_engine->ingestImu(ax, ay, az, gx, gy, gz, grav, label);
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
                if (name == QLatin1String("linearacceleration") || name == QLatin1String("accelerometeruncalibrated") || name == QLatin1String("accelerometer")) {
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
                m_engine->ingestImu(ax, ay, az, 0, 0, gz, grav, QStringLiteral("Sensor Logger"));
            }
            return;
        }
        return;
    }

    // CSV format (e.g. HyperIMU): "ax,ay,az" or "timestamp,ax,ay,az"
    const QList<QByteArray> parts = trimmed.split(',');
    if (parts.size() >= 3) {
        const int off = (parts.size() >= 4 && parts[0].size() > 8) ? 1 : 0;
        bool ok1 = false, ok2 = false, ok3 = false;
        const double ax = parts[off].trimmed().toDouble(&ok1);
        const double ay = parts[off + 1].trimmed().toDouble(&ok2);
        const double az = parts[off + 2].trimmed().toDouble(&ok3);
        if (ok1 && ok2 && ok3 && std::isfinite(ax) && std::isfinite(ay) && std::isfinite(az)) {
            bool okGz = false;
            const double gz = (parts.size() > off + 5) ? parts[off + 5].trimmed().toDouble(&okGz) : 0.0;
            m_engine->ingestImu(ax, ay, az, 0, 0, (okGz && std::isfinite(gz)) ? gz : 0.0, true, QStringLiteral("UDP CSV"));
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
  <p>想要<b>縮小 App 或關螢幕</b>省電持續傳送，請安裝下方 Android 背景版 APK（25Hz 活躍／靜止自動降 1Hz）。</p>
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
