#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

class MotionEngine;

class SensorServer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lanIp READ lanIp NOTIFY endpointsChanged)
    Q_PROPERTY(QString tailscaleIp READ tailscaleIp NOTIFY endpointsChanged)
    Q_PROPERTY(QString lanUrl READ lanUrl NOTIFY endpointsChanged)
    Q_PROPERTY(QString tailscaleUrl READ tailscaleUrl NOTIFY endpointsChanged)
    Q_PROPERTY(QString activeQrPath READ activeQrPath NOTIFY endpointsChanged)
    Q_PROPERTY(int qrRevision READ qrRevision NOTIFY endpointsChanged)
    Q_PROPERTY(bool useTailscaleQr READ useTailscaleQr WRITE setUseTailscaleQr NOTIFY endpointsChanged)
    Q_PROPERTY(int httpsPort READ httpsPort CONSTANT)
    Q_PROPERTY(int udpPort READ udpPort CONSTANT)
    Q_PROPERTY(bool httpsReady READ httpsReady NOTIFY endpointsChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY endpointsChanged)

public:
    explicit SensorServer(MotionEngine *engine, int httpsPort = 8765, int udpPort = 8766,
                          QObject *parent = nullptr);

    QString lanIp() const { return m_lanIp; }
    QString tailscaleIp() const { return m_tailscaleIp; }
    QString lanUrl() const;
    QString tailscaleUrl() const;
    QString activeQrPath() const;
    int qrRevision() const { return m_qrRevision; }
    bool useTailscaleQr() const { return m_useTailscaleQr; }
    void setUseTailscaleQr(bool v);

    int httpsPort() const { return m_httpsPort; }
    int udpPort() const { return m_udpPort; }
    bool httpsReady() const { return m_httpsReady; }
    QString statusMessage() const { return m_statusMessage; }
    int activeClientCount() const { return m_clients.size(); }

    Q_INVOKABLE void refreshNetwork();

signals:
    void endpointsChanged();

private:
    struct ClientState {
        bool isWebSocket = false;
        QByteArray buffer;
        qint64 lastActivityMs = 0;
    };

    void startUdp();
    void startHttps();
    bool ensureSelfSignedCert(const QString &certPath, const QString &keyPath);
    void regenerateQrCodesAsync();
    void spawnQrJob(const QString &url, const QString &outPath, bool isTailscale);
    void handleUdpDatagrams();
    void handleClientData(QSslSocket *sock);
    bool handleHttpRequest(QSslSocket *sock, const QByteArray &req);
    bool handleWebSocketFrames(QSslSocket *sock, ClientState &st);
    void removeClient(QSslSocket *sock);
    void sweepIdleClients();
    void parseImuPayload(const QByteArray &payload, const QString &source);
    QByteArray buildPhoneHtml() const;

    MotionEngine *m_engine;
    QUdpSocket m_udp;
    QSslServer m_https;
    QHash<QSslSocket *, ClientState> m_clients;
    QElapsedTimer m_uptimeClock;
    QTimer m_sweepTimer;

    QString m_lanIp;
    QString m_tailscaleIp;
    QString m_lanQrPath;
    QString m_tsQrPath;
    int m_qrRevision = 0;
    bool m_useTailscaleQr = false;
    int m_httpsPort = 8765;
    int m_udpPort = 8766;
    bool m_httpsReady = false;
    QString m_statusMessage;
};
