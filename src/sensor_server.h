#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include "qr_matrix.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

class MotionEngine;
class QNetworkDatagram;
class QFile;
class QProcess;

class SensorServer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lanIp READ lanIp NOTIFY endpointsChanged)
    Q_PROPERTY(QString tailscaleIp READ tailscaleIp NOTIFY endpointsChanged)
    Q_PROPERTY(QString lanUrl READ lanUrl NOTIFY endpointsChanged)
    Q_PROPERTY(QString tailscaleUrl READ tailscaleUrl NOTIFY endpointsChanged)
    Q_PROPERTY(QString activeQrUrl READ activeQrUrl NOTIFY endpointsChanged)
    Q_PROPERTY(bool useTailscaleQr READ useTailscaleQr WRITE setUseTailscaleQr NOTIFY endpointsChanged)
    Q_PROPERTY(int httpsPort READ httpsPort NOTIFY endpointsChanged)
    Q_PROPERTY(int udpPort READ udpPort CONSTANT)
    Q_PROPERTY(bool udpReady READ udpReady NOTIFY endpointsChanged)
    Q_PROPERTY(bool httpsReady READ httpsReady NOTIFY endpointsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY endpointsChanged)
    Q_PROPERTY(bool sourceLockEnabled READ sourceLockEnabled WRITE setSourceLockEnabled NOTIFY sourceLockEnabledChanged)
    Q_PROPERTY(QString lockedSource READ lockedSource NOTIFY lockedSourceChanged)

public:
    struct Options {
        quint16 httpsPort = 8765;
        quint16 udpPort = 8766;
        bool httpsEnabled = true;
        bool sourceLockEnabled = true;
        QString cacheDirectory;
        QString dataDirectory;
        int httpHeaderTimeoutMs = 5000;
    };

    struct Addresses {
        QString lan;
        QString tailscale;
    };

    explicit SensorServer(MotionEngine *engine, const Options &options, QObject *parent = nullptr);
    ~SensorServer() override;
    static Addresses networkAddresses();
    static QString pairingUrl(const QString &ip, quint16 port);

    QString lanIp() const { return m_lanIp; }
    QString tailscaleIp() const { return m_tailscaleIp; }
    QString lanUrl() const;
    QString tailscaleUrl() const;
    QString activeQrUrl() const;
    QrMatrix activeQr() const;
    bool useTailscaleQr() const { return m_useTailscaleQr; }
    void setUseTailscaleQr(bool v);

    quint16 httpsPort() const { return m_httpsPort; }
    quint16 udpPort() const { return m_udpPort; }
    bool udpReady() const { return m_udp.state() == QAbstractSocket::BoundState; }
    bool httpsReady() const { return m_httpsReady; }
    bool httpsStarting() const { return m_httpsStarting; }
    bool httpsBindFailed() const { return m_httpsBindFailed; }
    QString statusMessage() const;
    qsizetype activeClientCount() const { return m_clients.size(); }
    bool sourceLockEnabled() const { return m_options.sourceLockEnabled; }
    void setSourceLockEnabled(bool enabled);
    QString lockedSource() const;
    Q_INVOKABLE bool releaseSource();

    // Effective UI language for the pairing page ("zh-Hant" or "en"); set by
    // main() whenever the language setting changes. The page strings use
    // tr(), so they follow the installed translator at request time.
    void setLanguage(const QString &language) { m_language = language; }
    QString language() const { return m_language; }

    Q_INVOKABLE void refreshNetwork();

signals:
    void endpointsChanged();
    void sourceLockEnabledChanged();
    void lockedSourceChanged();

private:
    struct ClientState {
        bool isWebSocket = false;
        bool closing = false;
        QByteArray buffer;
        qint64 lastActivityMs = 0;
        qint64 headerDeadlineMs = 0;
    };

    void startUdp();
    void startHttps();
    void listenHttps(const QSslConfiguration &configuration);
    void finishHttps(const QString &status, bool ready = false);
    QString activePairingUrl() const;
    void handleUdpDatagrams();
    QString discoveryAddress(const QNetworkDatagram &request) const;
    void handleClientData(QSslSocket *sock);
    bool handleHttpRequest(QSslSocket *sock, const QByteArray &req);
    bool handleWebSocketFrames(QSslSocket *sock, ClientState &st);
    bool queueResponse(QSslSocket *sock, const QByteArray &data);
    void closeClient(QSslSocket *sock, const QByteArray &response = {});
    void armHeaderDeadline(QSslSocket *sock);
    void scheduleHeaderDeadline(QSslSocket *sock, qint64 deadlineMs, int delayMs);
    void streamApk(QSslSocket *sock, QFile *file);
    void removeClient(QSslSocket *sock);
    void sweepIdleClients();
    enum class Transport { Udp, Https, WebSocket };
    bool sourceAllowed(const QHostAddress &address) const;
    void ingestSample(const QHostAddress &address, double ax, double ay, double az,
                      double gx, double gy, double gz, bool gravityIncluded, const QString &label, bool idle = false);
    void parseImuPayload(const QByteArray &payload, Transport transport, const QHostAddress &address);
    QByteArray buildPhoneHtml() const;

    MotionEngine *m_engine;
    Options m_options;
    QUdpSocket m_udp;
    bool m_udpDrainQueued = false;
    QSslServer m_https;
    QPointer<QProcess> m_certificateProcess;
    QHash<QSslSocket *, ClientState> m_clients;
    QElapsedTimer m_uptimeClock;
    QElapsedTimer m_networkRefreshClock;
    QList<QNetworkInterface> m_interfaces;
    QTimer m_sweepTimer;
    QHostAddress m_activeSource;
    QHostAddress m_previousSource;
    QHostAddress m_releasedSource;
    qint64 m_releasedUntilMs = 0;
    struct DiscoveryWindow {
        qint64 startMs = 0;
        int replies = 0;
    };
    // Per requesting address, so one noisy host cannot use up everyone's replies.
    QHash<QHostAddress, DiscoveryWindow> m_discoveryWindows;

    QString m_lanIp;
    QString m_tailscaleIp;
    QString m_language = QStringLiteral("zh-Hant");
    int m_qrRevision = 0;
    bool m_useTailscaleQr = false;
    quint16 m_httpsPort;
    quint16 m_udpPort;
    bool m_httpsReady = false;
    bool m_httpsStarting;
    bool m_httpsBindFailed = false;
    QString m_httpsStatus;
};
