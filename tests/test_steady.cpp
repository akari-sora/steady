#include "../src/motion_engine.h"
#include "../src/sensor_server.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslSocket>
#include <QTimer>
#include <QUdpSocket>
#include <cmath>
#include <iostream>

namespace {
int g_failures = 0;

void expect(bool cond, const char *msg)
{
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        g_failures++;
    } else {
        std::cout << "PASS: " << msg << "\n";
    }
}

void processFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitEncryptedAsync(QSslSocket &cli, int timeoutMs)
{
    if (cli.isEncrypted()) return true;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&cli, &QSslSocket::encrypted, &loop, [&]() {
        ok = true;
        loop.quit();
    });
    QObject::connect(&cli, &QAbstractSocket::errorOccurred, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return ok;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 1. MotionEngine: NaN/Infinity rejection & magnitude clamping
    {
        MotionEngine eng;
        eng.setMode(QStringLiteral("phone"));
        eng.ingestImu(std::nan(""), 1.0, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        eng.ingestImu(1.0, INFINITY, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        expect(!eng.phoneConnected(), "NaN/Inf IMU packets are rejected before updating state");

        eng.ingestImu(999.0, -999.0, 0.0, 0.0, 0.0, 999.0, false, QStringLiteral("Test"));
        processFor(80);
        expect(eng.phoneConnected(), "Valid clamped IMU packet connects phone");
        expect(std::isfinite(eng.ax()) && std::isfinite(eng.ay()) && std::isfinite(eng.offsetX()),
               "Clamped extreme IMU values keep filter & kinematics finite");
    }

    // 2. MotionEngine: calibrate() idempotency & gravity reset
    {
        MotionEngine eng;
        eng.setMode(QStringLiteral("phone"));
        eng.ingestImu(2.0, -1.5, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        eng.calibrate();
        eng.calibrate(); // second click before next packet must not double-add bias
        eng.ingestImu(2.0, -1.5, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        processFor(80);
        expect(std::abs(eng.ax()) < 0.05 && std::abs(eng.ay()) < 0.05,
               "Repeated calibrate() is idempotent and zeroes linear offset");

        // gravityIncluded mode should not accumulate false bias
        eng.ingestImu(0.5, 0.5, 9.8, 0.0, 0.0, 0.0, true, QStringLiteral("Test"));
        eng.calibrate();
        eng.ingestImu(0.5, 0.5, 9.8, 0.0, 0.0, 0.0, true, QStringLiteral("Test"));
        processFor(80);
        expect(std::abs(eng.ax()) < 0.05 && std::abs(eng.ay()) < 0.05,
               "calibrate() with gravityIncluded resets gravity cleanly");
    }

    // 3. MotionEngine: idle-sleep stops timer when settled (0 Hz CPU wakeups)
    {
        MotionEngine eng;
        eng.setAutoHide(false);
        eng.setMode(QStringLiteral("sim"));
        eng.setSimPreset(QStringLiteral("manual"));
        eng.triggerImpulse(2.0, 0.0);
        expect(eng.isAnimationTimerActive(), "Animation timer wakes on impulse");
        processFor(3200);
        expect(!eng.isAnimationTimerActive(), "Animation timer stops automatically after settling (idle-sleep)");
        eng.triggerImpulse(-1.5, 0.0);
        expect(eng.isAnimationTimerActive(), "Animation timer wakes immediately on new motion");
    }

    // 4. MotionEngine: disconnect watchdog still runs when overlayEnabled == false
    {
        MotionEngine eng;
        eng.setOverlayEnabled(false);
        eng.ingestImu(0.2, 0.0, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        expect(eng.phoneConnected(), "Phone connects even while overlay disabled");
        // Watchdog timeout is 3.5s -> after 4.6s with no packets, must disconnect
        processFor(4600);
        expect(!eng.phoneConnected(), "Disconnect watchdog clears phoneConnected even when overlayEnabled == false");
    }

    // 5. SensorServer: UDP discovery + NaN CSV rejection + HTTPS/WS overflow rejection
    {
        MotionEngine eng;
        eng.setMode(QStringLiteral("phone"));
        SensorServer srv(&eng, 0, 0);
        expect(srv.httpsReady() && srv.httpsPort() > 0 && srv.udpPort() > 0,
               "SensorServer binds ephemeral HTTPS and UDP ports for testing");

        // UDP discovery
        QUdpSocket udp;
        udp.bind(QHostAddress::LocalHost, 0);
        udp.writeDatagram(QByteArrayLiteral("{\"cmd\":\"discover\"}"), QHostAddress::LocalHost, quint16(srv.udpPort()));
        processFor(120);
        expect(udp.hasPendingDatagrams(), "UDP discovery receives reply");
        if (udp.hasPendingDatagrams()) {
            QByteArray rep(int(udp.pendingDatagramSize()), '\0');
            udp.readDatagram(rep.data(), rep.size());
            const QJsonObject obj = QJsonDocument::fromJson(rep).object();
            expect(obj.value(QStringLiteral("steady")).toBool(false), "UDP discovery reply contains steady=true");
        }

        // UDP NaN CSV must not connect/poison
        udp.writeDatagram(QByteArrayLiteral("nan,0,0"), QHostAddress::LocalHost, quint16(srv.udpPort()));
        processFor(80);
        expect(!eng.phoneConnected(), "Malformed UDP CSV 'nan,0,0' is rejected");

        // HTTPS negative Content-Length must be rejected cleanly
        {
            QSslSocket cli;
            cli.setPeerVerifyMode(QSslSocket::VerifyNone);
            cli.connectToHostEncrypted(QStringLiteral("127.0.0.1"), quint16(srv.httpsPort()));
            expect(waitEncryptedAsync(cli, 3000), "TLS test client connects for negative Content-Length check");
            cli.write("POST /imu HTTP/1.1\r\nHost: localhost\r\nContent-Length: -50\r\n\r\n");
            cli.flush();
            processFor(150);
            const QByteArray resp = cli.readAll();
            expect(resp.contains("400 Bad Request"), "Negative Content-Length returns 400 Bad Request");
        }

        // WebSocket valid masked IMU text frame + Ping/Pong + abrupt disconnect (UAF regression)
        {
            QSslSocket cli;
            cli.setPeerVerifyMode(QSslSocket::VerifyNone);
            cli.connectToHostEncrypted(QStringLiteral("127.0.0.1"), quint16(srv.httpsPort()));
            expect(waitEncryptedAsync(cli, 3000), "TLS test client connects for WS IMU + Ping/Pong check");
            cli.write("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
            cli.flush();
            processFor(120);
            expect(cli.readAll().contains("101 Switching Protocols"), "WS upgrade for valid frame test succeeds");

            // Masked text frame carrying {"ax":1.2,"ay":0.3,"az":0,"gz":0.1,"grav":false}
            const QByteArray json = R"({"ax":1.2,"ay":0.3,"az":0,"gz":0.1,"grav":false})";
            const char mask[4] = {0x12, 0x34, 0x56, 0x78};
            QByteArray txtFrame;
            txtFrame.append(char(0x81));
            txtFrame.append(char(0x80 | json.size()));
            txtFrame.append(mask, 4);
            for (int i = 0; i < json.size(); ++i) {
                txtFrame.append(char(json[i] ^ mask[i % 4]));
            }
            // Also append a masked Ping frame (opcode 0x9, len 0) in the same TCP segment
            txtFrame.append(char(0x89));
            txtFrame.append(char(0x80));
            txtFrame.append(mask, 4);

            cli.write(txtFrame);
            cli.flush();
            processFor(150);
            const QByteArray pong = cli.readAll();
            expect(eng.phoneConnected(), "Valid masked WS IMU frame updates MotionEngine");
            expect(pong.size() >= 2 && static_cast<quint8>(pong[0]) == 0x8A,
                   "WS Ping receives RFC6455 Pong reply");

            // Abruptly abort client mid-session to verify no UAF/crash
            cli.abort();
            processFor(100);
        }

        // WebSocket 64-bit UINT64_MAX length overflow must disconnect immediately without hanging
        {
            QSslSocket cli;
            cli.setPeerVerifyMode(QSslSocket::VerifyNone);
            cli.connectToHostEncrypted(QStringLiteral("127.0.0.1"), quint16(srv.httpsPort()));
            expect(waitEncryptedAsync(cli, 3000), "TLS test client connects for WS overflow check");
            cli.write("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
            cli.flush();
            processFor(150);
            const QByteArray upgrade = cli.readAll();
            expect(upgrade.contains("101 Switching Protocols"), "WebSocket upgrade succeeds");

            // Send crafted WS frame: FIN+Text (0x81), Masked+127 (0xFF), 8-byte len = 0xFFFFFFFFFFFFFFFF, 4-byte mask
            QByteArray badFrame;
            badFrame.append(char(0x81));
            badFrame.append(char(0xFF));
            for (int i = 0; i < 8; ++i) badFrame.append(char(0xFF));
            for (int i = 0; i < 4; ++i) badFrame.append(char(0x00));
            cli.write(badFrame);
            cli.flush();
            processFor(150);
            expect(cli.state() != QAbstractSocket::ConnectedState,
                   "Oversized 64-bit WS frame is rejected and disconnects without looping");
        }

        processFor(100);
        expect(srv.activeClientCount() == 0, "All closed/rejected TLS clients are cleaned up from m_clients");
    }

    if (g_failures > 0) {
        std::cerr << "\nFAILED: " << g_failures << " assertion(s)\n";
        return 1;
    }
    std::cout << "\nALL TESTS PASSED\n";
    return 0;
}
