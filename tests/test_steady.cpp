#include "../src/app_paths.h"
#include "../src/local_control.h"
#include "../src/motion_engine.h"
#include "../src/sensor_server.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QNetworkDatagram>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QUdpSocket>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
int g_failures = 0;
QStringList *g_capturedMessages = nullptr;

void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    if (g_capturedMessages) g_capturedMessages->append(message);
}

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
bool finishProcess(QProcess &process, int timeout = 5000)
{
    if (process.state() == QProcess::NotRunning || process.waitForFinished(timeout)) {
        return process.error() != QProcess::FailedToStart && process.exitStatus() == QProcess::NormalExit;
    }
    process.kill();
    process.waitForFinished(2000);
    return false;
}

QList<QJsonObject> telemetryLines(const QByteArray &output)
{
    QList<QJsonObject> rows;
    for (const QByteArray &line : output.trimmed().split('\n')) {
        const auto doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) return {};
        const auto row = doc.object();
        for (QLatin1String key : {QLatin1String("t"), QLatin1String("ax"), QLatin1String("ay"),
                                  QLatin1String("offsetX"), QLatin1String("offsetY"), QLatin1String("alpha"),
                                  QLatin1String("hz")}) {
            if (!row.value(key).isDouble()) return {};
        }
        if (!row.value("connected").isBool() || !row.value("source").isString()) return {};
        for (unsigned char c : line) {
            if (c >= 128) return {};
        }
        rows.append(row);
    }
    return rows;
}

void waitForHttps(SensorServer &server)
{
    if (!server.httpsStarting()) return;
    QEventLoop loop;
    QObject::connect(&server, &SensorServer::endpointsChanged, &loop, [&]() {
        if (!server.httpsStarting()) loop.quit();
    });
    QTimer::singleShot(11000, &loop, &QEventLoop::quit);
    loop.exec();
}

QHostAddress bindSecondAddress(QAbstractSocket &socket)
{
    const auto addresses = SensorServer::networkAddresses();
    const QStringList candidates{"127.0.0.2", addresses.lan, addresses.tailscale};
    for (const auto &candidate : candidates) {
        const QHostAddress address(candidate);
        if (!address.isNull() && socket.bind(address, 0)) return address;
    }
    std::cout << "SKIP: second local IPv4 address unavailable\n";
    return {};
}

void testUdpParsing()
{
    SensorServer::Options options;
    options.udpPort = 0;
    options.httpsEnabled = false;
    const QHostAddress loopback = QHostAddress::LocalHost;
    QUdpSocket sender;
    {
        MotionEngine engine(MotionEngine::Timing::Manual);
        SensorServer server(&engine, options);
        sender.writeDatagram(R"({"ax":1.2,"ay":0,"grav":false,"src":"<b>Phone</b>"})", loopback, server.udpPort());
        processFor(40);
        expect(engine.imuSource() == QStringLiteral("<b>Phone</b>"), "Packet source markup is kept verbatim");
        sender.writeDatagram(R"({"ax":1.2,"ay":0,"grav":false,"src":"123456789012345678901234567890123456"})",
                             loopback, server.udpPort());
        processFor(40);
        expect(engine.imuSource() == QStringLiteral("12345678901234567890123456789012"),
               "Packet source is capped at 32 UTF-16 units");
        sender.writeDatagram(R"({"ax":1.2,"grav":false,"src":"A\nB\u202e\u2028C"})",
                             loopback, server.udpPort());
        processFor(40);
        expect(engine.imuSource() == QStringLiteral("A B  C"),
               "Packet source control, format and line-separator characters are neutralized");
    }
    for (bool timestamp : {false, true}) {
        MotionEngine engine(MotionEngine::Timing::Manual);
        SensorServer server(&engine, options);
        sender.writeDatagram("0,0,9.81", loopback, server.udpPort());
        processFor(40);
        engine.advance(0.040);
        const QByteArray sample = timestamp ? "7,1.25,-0.8,9.81,0.1,0.2,0.4" : "1.25,-0.8,9.81";
        sender.writeDatagram(sample, loopback, server.udpPort());
        processFor(40);
        engine.advance(0.025);
        const double gravityRemainder = std::exp(-0.040 / 20.0);
        const double filter = 1.0 - std::exp(-0.025 * 18.0);
        const double expectedX = (1.25 * gravityRemainder + (timestamp ? 0.85 * 0.4 : 0.0) - 0.14) * filter;
        const double expectedY = (-0.8 * gravityRemainder + 0.14) * filter;
        expect(std::abs(engine.ax() - expectedX) < 0.001 && std::abs(engine.ay() - expectedY) < 0.001,
               timestamp ? "Seven-column CSV uses short timestamp, axes and gyro"
                         : "Three-column CSV uses the three acceleration axes");
    }

    QFile qml(QStringLiteral(":/Main.qml"));
    expect(qml.open(QIODevice::ReadOnly), "Control panel QML is available to the tests");
    const QByteArray source = qml.readAll();
    const auto labelStart = source.indexOf("+ motion.imuSource");
    const auto nextLabel = source.indexOf("Label {", labelStart + 1);
    expect(labelStart >= 0 && nextLabel > labelStart
               && source.mid(labelStart, nextLabel - labelStart).contains("textFormat: Text.PlainText"),
           "Control panel renders phone source as plain text");
    expect(labelStart >= 0 && nextLabel > labelStart
               && source.mid(labelStart, nextLabel - labelStart).contains("maximumLineCount: 1"),
           "Control panel limits the phone source to one line");
    expect(source.contains("text: server.statusMessage") && source.contains("visible: server.activeQrUrl.length > 0")
               && source.contains("visible: server.lanUrl.length > 0 || server.tailscaleUrl.length > 0"),
           "Control panel binds status and hides unavailable pairing URLs and QR");
}

void testDiscoveryRateLimit()
{
    MotionEngine engine;
    SensorServer::Options options;
    options.httpsEnabled = false;
    options.udpPort = 0;
    SensorServer server(&engine, options);
    QUdpSocket client;
    client.bind(QHostAddress::LocalHost, 0);
    const QByteArray request(R"({"cmd":"discover"})");
    int replies = 0;
    for (int i = 0; i < 40; ++i) {
        client.writeDatagram(request, QHostAddress::LocalHost, server.udpPort());
        processFor(15);
        while (client.hasPendingDatagrams()) {
            client.receiveDatagram();
            ++replies;
        }
    }
    expect(replies == 32, "Discovery replies are capped per one-second window");
    processFor(1050);
    client.writeDatagram(request, QHostAddress::LocalHost, server.udpPort());
    processFor(40);
    expect(client.hasPendingDatagrams(), "Discovery remains available after the rate window resets");
    while (client.hasPendingDatagrams()) client.receiveDatagram();

    // Once this address is the connected source its probes must always be answered:
    // the phone stops streaming when its discover probes go unanswered.
    client.writeDatagram(R"({"ax":0.5,"ay":0,"az":0,"grav":false})", QHostAddress::LocalHost, server.udpPort());
    processFor(60);
    expect(!server.lockedSource().isEmpty(), "A sample makes the sender the connected source");
    int connectedReplies = 0;
    for (int i = 0; i < 40; ++i) {
        client.writeDatagram(request, QHostAddress::LocalHost, server.udpPort());
        processFor(15);
        while (client.hasPendingDatagrams()) {
            client.receiveDatagram();
            ++connectedReplies;
        }
    }
    expect(connectedReplies == 40, "The connected source's discovery probes are never rate limited");
}

void testLocalControl()
{
#ifdef Q_OS_WIN
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/steady-lc-XXXXXX"));
#else
    QTemporaryDir directory(QStringLiteral("/tmp/steady-lc-XXXXXX"));
#endif
    const QString scope = QStringLiteral("t-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString marker = directory.filePath(QStringLiteral("child-ready"));
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--local-control-stale-child"), directory.path(), scope, marker});
    expect(finishProcess(child, 3000) && child.exitCode() == 0 && QFileInfo::exists(marker),
           "Abrupt local-control child leaves a stale endpoint for recovery");

    LocalControl control(scope, directory.path());
    expect(control.tryOwn() && control.listen("p", [](char) { return LocalControl::Reply::Accepted; }),
           "Local control recovers a stale lock and socket");

    const auto exchangeLocal = [&](char command) {
        QLocalSocket socket;
        socket.connectToServer(control.serverName());
        processFor(30);
        if (socket.state() != QLocalSocket::ConnectedState || !socket.putChar(command)) return char('?');
        socket.flush();
        processFor(30);
        char response = '?';
        socket.getChar(&response);
        return response;
    };
    expect(exchangeLocal('x') == '0', "Local control rejects an unsupported command");

    std::vector<std::unique_ptr<QLocalSocket>> idleClients;
    for (int i = 0; i < 20; ++i) {
        auto socket = std::make_unique<QLocalSocket>();
        socket->connectToServer(control.serverName());
        idleClients.push_back(std::move(socket));
    }
    processFor(120);
    expect(control.activeClientCount() > 0 && control.activeClientCount() <= 8,
           "Local control caps concurrent idle clients");
    for (auto &socket : idleClients) socket->abort();
    processFor(40);
    expect(exchangeLocal('p') == '1', "Local control accepts commands after excess clients disconnect");

#ifndef Q_OS_WIN
    const QString occupiedScope = scope + QStringLiteral("-occupied");
    LocalControl occupied(occupiedScope, directory.path());
    const bool owns = occupied.tryOwn();
    const bool madeDirectory = QDir().mkdir(occupied.serverName());
    expect(owns && madeDirectory && !occupied.listen("p", [](char) { return LocalControl::Reply::Accepted; }),
           "An occupied local socket path makes control setup fail cleanly");
#else
    std::cout << "SKIP: filesystem socket-path occupation is not applicable to Windows named pipes\n";
#endif
}

void testUdpBurst()
{
    MotionEngine engine;
    SensorServer::Options options;
    options.httpsEnabled = false;
    options.udpPort = 0;
    SensorServer server(&engine, options);
    constexpr int packets = 80;
    int parsed = 0;
    int firstYield = -1;
    int frames = 0;
    QObject::connect(&engine, &MotionEngine::imuStatusChanged, &server, [&]() {
        if (++parsed == 1) QTimer::singleShot(0, &server, [&]() { firstYield = parsed; });
    });
    QObject::connect(&engine, &MotionEngine::frameUpdated, &server, [&]() { ++frames; });

    QUdpSocket sender;
    for (int i = 0; i < packets; ++i) {
        const QByteArray data = "{\"ax\":2.4,\"grav\":false,\"src\":\"" + QByteArray::number(i) + "\"}";
        sender.writeDatagram(data, QHostAddress::LocalHost, server.udpPort());
    }
    processFor(160);
    expect(firstYield > 0 && firstYield <= 32 && firstYield < parsed && frames > 0 && engine.offsetX() < -0.5,
           "UDP burst yields to the event loop and the animation timer moves the dots");
    std::cout << "UDP burst first yield=" << firstYield << " parsed=" << parsed << " animation frames=" << frames << '\n';
}

void testSourceLock()
{
    MotionEngine engine(MotionEngine::Timing::Manual);
    SensorServer::Options options;
    options.udpPort = 0;
    options.httpsEnabled = false;
    SensorServer server(&engine, options);
    QUdpSocket first, second;
    const QHostAddress loopback = QHostAddress::LocalHost;
    const QHostAddress other = bindSecondAddress(second);
    const auto send = [&](QUdpSocket &socket, const QHostAddress &host, const QByteArray &payload) {
        socket.writeDatagram(payload, host, server.udpPort());
        processFor(40);
    };
    const QByteArray a = R"({"ax":2,"grav":false,"src":"one"})";
    const QByteArray b = R"({"ax":-2,"grav":false,"src":"two"})";
    const QByteArray calibrate = R"({"cmd":"calibrate"})";
    send(first, loopback, a);
    expect(server.sourceLockEnabled() && server.lockedSource() == loopback.toString(),
           "First valid sample owns the default source lock");
    if (other.isNull()) {
        server.releaseSource();
        send(first, loopback, a);
        expect(!engine.phoneConnected() && server.lockedSource().isEmpty(),
               "Manual release temporarily excludes the released IPv4 address");
        return;
    }
    engine.advance(2.0);
    const QString hint = engine.calibrationHint();
    send(second, other, b);
    send(second, other, calibrate);
    engine.advance(0.1);
    expect(engine.imuSource() == QStringLiteral("one") && engine.ax() > 1 && engine.calibrationHint() == hint,
           "A different address cannot drive the engine or calibrate while locked");
    send(second, other, R"({"cmd":"discover"})");
    expect(second.hasPendingDatagrams(), "Discovery remains open to another address while locked");
    engine.advance(1.4);
    expect(!engine.phoneConnected() && server.lockedSource().isEmpty(),
           "Ignored packets do not extend the source lock past the watchdog");
    send(second, other, b);
    expect(engine.imuSource() == QStringLiteral("two") && server.lockedSource() == other.toString(),
           "A different source can claim the lock after the watchdog");
    server.releaseSource();
    expect(!engine.phoneConnected() && server.lockedSource().isEmpty(), "Release clears the lock immediately");
    send(second, other, b);
    expect(!engine.phoneConnected() && server.lockedSource().isEmpty(),
           "Manual release temporarily excludes the released IPv4 address");
    send(first, loopback, a);
    send(first, loopback, calibrate);
    expect(engine.calibrationHint() != hint, "The locked source can calibrate");
    server.setSourceLockEnabled(false);
    send(second, other, b);
    expect(engine.imuSource() == QStringLiteral("two") && server.lockedSource().isEmpty(),
           "Disabling the lock allows another source immediately");
}

void testGravityResponse()
{
    MotionEngine engine(MotionEngine::Timing::Manual);
    const auto feed = [&](double x) {
        engine.ingestImu(x, 0, 9.81, 0, 0, 0, true, QStringLiteral("Gravity test"));
        engine.advance(0.040);
    };
    feed(0);
    double atTwoSeconds = 0;
    for (int i = 1; i <= 75; ++i) {
        feed(3);
        if (i == 50) atTwoSeconds = engine.ax();
    }
    double afterRelease = 0;
    for (int i = 0; i < 75; ++i) {
        feed(0);
        afterRelease = std::min(afterRelease, engine.ax());
    }
    std::cout << "Gravity 3 m/s^2 step: at 2 s=" << atTwoSeconds
              << ", minimum after release=" << afterRelease << '\n';
    expect(atTwoSeconds >= 0.80 * 3, "Gravity filter preserves at least 80% of sustained input at 2 s");
    expect(afterRelease >= -0.30, "Gravity filter keeps release recoil at or above -0.30 m/s^2");

    MotionEngine linear(MotionEngine::Timing::Manual);
    linear.ingestImu(3, 0, 9.81, 0, 0, 0, false, QStringLiteral("Linear test"));
    linear.advance(0.025);
    const double expected = (3.0 - 0.14) * (1.0 - std::exp(-0.025 * 18.0));
    expect(std::abs(linear.ax() - expected) < 1e-9,
           "Linear samples still use the original dead zone and filter");
}

void testIdleKeepalive()
{
    MotionEngine engine(MotionEngine::Timing::Manual);
    SensorServer::Options options;
    options.udpPort = 0;
    options.httpsEnabled = false;
    SensorServer server(&engine, options);
    QUdpSocket sender;
    const auto send = [&](const QByteArray &payload) {
        sender.writeDatagram(payload, QHostAddress::LocalHost, server.udpPort());
        processFor(20);
    };
    send(R"({"ax":2,"grav":false})");
    engine.advance(0.6);
    expect(engine.isAnimationTimerActive() && engine.ax() > 1, "Unmarked motion starts the timer");
    const QByteArray keepalive = R"({"ax":0.22,"ay":0.19,"gz":0.08,"grav":false,"idle":true})";
    for (int i = 0; i < 9; ++i) {
        send(keepalive);
        engine.advance(1);
    }
    expect(engine.phoneConnected() && !engine.isAnimationTimerActive()
               && engine.ax() == 0 && engine.ay() == 0 && engine.currentAlpha() == 0,
           "Biased idle keepalives retain connection and stop animation within 9 s");
    send(R"({"ax":0.22,"ay":0.19,"gz":0.08,"grav":false})");
    engine.advance(0.1);
    expect(engine.isAnimationTimerActive() && engine.ax() > 0, "The same values without idle still wake the timer");

    MotionEngine gravity(MotionEngine::Timing::Manual);
    gravity.ingestImu(0, 0, 9.81, 0, 0, 0, true, QStringLiteral("Gravity test"));
    for (int i = 0; i < 4; ++i) {
        gravity.advance(1);
        gravity.ingestImu(30, 0, 8, 0, 0, 0, true, QStringLiteral("Gravity test"), true);
    }
    gravity.ingestImu(0, 0, 9.81, 0, 0, 0, true, QStringLiteral("Gravity test"));
    gravity.advance(0.1);
    expect(gravity.ax() == 0 && !gravity.isAnimationTimerActive(),
           "Idle samples leave the gravity estimate untouched");

    send(R"({"ax":2.5,"ay":1,"grav":false,"idle":true})");
    send(R"({"cmd":"calibrate"})");
    send(R"({"ax":2.5,"ay":1,"grav":false})");
    engine.advance(0.4);
    expect(engine.ax() == 0 && engine.ay() == 0, "Parked calibration learns the last linear idle sample");
    engine.calibrate();
    send(R"({"ax":0,"ay":0,"grav":false,"idle":true})");
    send(R"({"cmd":"calibrate"})");
    send(R"({"ax":0,"ay":0,"grav":false})");
    engine.advance(0.4);
    expect(engine.ax() == 0 && engine.ay() == 0, "Parked calibration clears a previous linear bias");

    gravity.ingestImu(1.5, 1, 9.5, 0, 0, 0, true, QStringLiteral("Gravity test"), true);
    gravity.calibrate();
    gravity.ingestImu(3.5, 1, 9.5, 0, 0, 0, true, QStringLiteral("Gravity test"));
    gravity.advance(0.5);
    expect(gravity.ax() > 1.5, "Parked gravity calibration seeds from idle before the first acceleration");
}

void testSettings()
{
    QTemporaryDir folder;
    const QString path = folder.filePath(QStringLiteral("settings.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        MotionEngine engine(MotionEngine::Timing::Manual);
        engine.loadSettings(settings);
        expect(engine.mode() == QLatin1String("phone") && engine.simPreset() == QLatin1String("city")
                   && engine.dotPairs() == 4 && engine.autoHide(), "Empty settings retain the first-run defaults");
        expect(!engine.isAnimationTimerActive() && engine.currentAlpha() == 0,
               "Fresh phone mode starts without animation or visible dots");
        engine.setMode(QStringLiteral("sim"));
        engine.setSimPreset(QStringLiteral("mountain"));
        engine.setDotPairs(6);
        engine.setDotSize(19);
        engine.setOpacity(0.45);
        engine.setSensitivity(1.8);
        engine.setEdgeMargin(43);
        engine.setAutoHide(false);
        engine.setUseAccentColor(true);
        engine.saveSettings(settings);
    }
    QSettings settings(path, QSettings::IniFormat);
    MotionEngine engine(MotionEngine::Timing::Manual);
    engine.loadSettings(settings);
    expect(engine.mode() == QLatin1String("sim") && engine.simPreset() == QLatin1String("mountain")
               && engine.dotPairs() == 6 && engine.dotSize() == 19 && engine.opacity() == 0.45
               && engine.sensitivity() == 1.8 && engine.edgeMargin() == 43
               && !engine.autoHide() && engine.useAccentColor(), "All visual and motion settings survive restart");

    settings.setValue(QStringLiteral("dotPairs"), QStringLiteral("broken"));
    settings.setValue(QStringLiteral("dotSize"), QStringLiteral("broken"));
    settings.setValue(QStringLiteral("opacity"), QStringLiteral("broken"));
    settings.setValue(QStringLiteral("sensitivity"), QStringLiteral("broken"));
    settings.setValue(QStringLiteral("margin"), QStringLiteral("broken"));
    MotionEngine corrupted(MotionEngine::Timing::Manual);
    corrupted.loadSettings(settings);
    expect(corrupted.dotPairs() == 4 && corrupted.dotSize() == 12.0 && corrupted.opacity() == 0.80
               && corrupted.sensitivity() == 1.0 && corrupted.edgeMargin() == 26.0,
           "Nonnumeric saved settings preserve numeric defaults");
}

void testSimulationTimeout()
{
    MotionEngine engine(MotionEngine::Timing::Manual);
    engine.setMode(QStringLiteral("sim"));
    engine.advance(300);
    engine.noteInteraction();
    engine.advance(599.999);
    expect(!engine.simulationPaused() && engine.isAnimationTimerActive(),
           "Continuous simulation stays active until ten minutes after interaction");
    engine.advance(0.001);
    expect(engine.simulationPaused() && engine.mode() == QLatin1String("sim"),
           "Continuous simulation pauses at its inactivity deadline and keeps the mode");
    engine.advance(10);
    expect(!engine.isAnimationTimerActive() && engine.currentAlpha() == 0,
           "Paused simulation fades out and stops the animation timer");
    engine.resumeSimulation();
    expect(!engine.simulationPaused() && engine.isAnimationTimerActive(),
           "Resuming a paused simulation restarts animation");

    MotionEngine cliEngine(MotionEngine::Timing::Manual, MotionEngine::SimulationTimeout::Disabled);
    cliEngine.setMode(QStringLiteral("sim"));
    cliEngine.advance(601);
    expect(!cliEngine.simulationPaused() && cliEngine.isAnimationTimerActive(),
           "A CLI simulation does not pause after ten minutes");
}

bool connectTls(QSslSocket &socket, const QHostAddress &address, quint16 port)
{
    socket.setPeerVerifyMode(QSslSocket::VerifyNone);
    socket.connectToHostEncrypted(address.toString(), port);
    return waitEncryptedAsync(socket, 3000);
}

QByteArray exchange(QSslSocket &socket, const QByteArray &data)
{
    socket.write(data);
    socket.flush();
    processFor(120);
    return socket.readAll();
}

bool upgradeSocket(QSslSocket &socket)
{
    return exchange(socket, "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n").startsWith("HTTP/1.1 101 ");
}

QByteArray clientFrame(quint8 header, const QByteArray &payload = {})
{
    const QByteArray mask = QByteArray::fromHex("12345678");
    QByteArray frame;
    frame += char(header);
    frame += char(0x80 | payload.size());
    frame += mask;
    for (qsizetype i = 0; i < payload.size(); ++i) frame += char(payload[i] ^ mask[i % 4]);
    return frame;
}

void testTlsRegressions(const SensorServer::Options &options)
{
    {
        MotionEngine deadlineEngine(MotionEngine::Timing::Manual);
        SensorServer::Options deadlineOptions = options;
        // The deadline starts when the server side of the handshake completes. The
        // client can report "encrypted" noticeably later on a cold first run
        // (seen on Windows), so the deadline must leave room for that.
        deadlineOptions.httpHeaderTimeoutMs = 1500;
        SensorServer deadlineServer(&deadlineEngine, deadlineOptions);
        waitForHttps(deadlineServer);
        QSslSocket partial;
        const bool connected = deadlineServer.httpsReady()
            && connectTls(partial, QHostAddress::LocalHost, deadlineServer.httpsPort());
        if (connected) {
            QEventLoop wait;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&partial, &QSslSocket::disconnected, &wait, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, &wait, &QEventLoop::quit);
            partial.write("GET /");
            partial.flush();
            timeout.start(6000);
            wait.exec();
        }
        // The client can see the disconnect before the server has handled its own
        // socket's disconnected event; give the server up to one more second.
        for (int i = 0; i < 100 && deadlineServer.activeClientCount() != 0; ++i) {
            QEventLoop spin;
            QTimer::singleShot(10, &spin, &QEventLoop::quit);
            spin.exec();
        }
        const bool serverDroppedClient = deadlineServer.activeClientCount() == 0;
        expect(connected && serverDroppedClient,
               "An incomplete HTTP header is disconnected at its fixed deadline");
        partial.abort();
    }

    QTemporaryDir apkData;
    constexpr qint64 apkSize = 16 * 1024 * 1024;
    constexpr qint64 changedTailSize = 64 * 1024;
    const QString apkPath = QDir(apkData.path()).filePath(QStringLiteral("steady-sender.apk"));
    QFile apk(apkPath);
    const bool apkPrepared = apkData.isValid() && apk.open(QIODevice::WriteOnly)
        && apk.resize(apkSize) && apk.seek(apkSize - changedTailSize)
        && apk.write(QByteArray(changedTailSize, 'B')) == changedTailSize;
    apk.close();
    expect(apkPrepared, "Prepare a multi-megabyte APK streaming fixture");

    SensorServer::Options serverOptions = options;
    serverOptions.dataDirectory = apkData.path();
    MotionEngine engine(MotionEngine::Timing::Manual);
    QSslSocket openHttp, openWebSocket;
    auto server = std::make_unique<SensorServer>(&engine, serverOptions);
    waitForHttps(*server);
    const QHostAddress loopback = QHostAddress::LocalHost;
    const auto port = server->httpsPort();
    if (!server->httpsReady()) {
        expect(false, "Start TLS server for protocol and teardown regressions");
        return;
    }
    for (const QByteArray &headers : {QByteArray("Content-Length: 0\r\nContent-Length: 0"),
                                      QByteArray("Transfer-Encoding: chunked")}) {
        QSslSocket invalid;
        const bool connected = connectTls(invalid, loopback, port);
        const auto response = connected ? exchange(invalid, "POST /imu HTTP/1.1\r\n" + headers + "\r\n\r\n")
                                        : QByteArray();
        expect(response.startsWith("HTTP/1.1 400 ") && response.contains("Connection: close") &&
                   invalid.state() == QAbstractSocket::UnconnectedState,
               "Ambiguous HTTP request framing receives 400 and disconnects");
    }
    for (const QByteArray &frame : {QByteArray::fromHex("8100"), clientFrame(0x01), clientFrame(0x80)}) {
        QSslSocket invalid;
        const bool connected = connectTls(invalid, loopback, port) && upgradeSocket(invalid);
        if (connected) exchange(invalid, frame);
        expect(connected && invalid.state() == QAbstractSocket::UnconnectedState,
               "Unmasked, fragmented or continuation WebSocket frame disconnects");
    }

    QUdpSocket source;
    source.writeDatagram(R"({"ax":2,"grav":false,"src":"owner"})", loopback, server->udpPort());
    processFor(40);
    const QString hint = engine.calibrationHint();
    QSslSocket otherHttp;
    const auto other = bindSecondAddress(otherHttp);
    if (!other.isNull()) {
        const bool connected = connectTls(otherHttp, other, port);
        const auto rejected = connected ? exchange(otherHttp, "POST /calibrate HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
                                        : QByteArray();
        expect(rejected.startsWith("HTTP/1.1 200 ") && engine.calibrationHint() == hint,
               "HTTPS calibration from another address is ignored while locked");
        expect(connected && upgradeSocket(otherHttp), "Second address can open WSS while locked");
        exchange(otherHttp, clientFrame(0x81, R"({"ax":3,"grav":false,"src":"other"})") +
                            clientFrame(0x81, R"({"cmd":"calibrate"})"));
        expect(engine.imuSource() == QStringLiteral("owner") && engine.calibrationHint() == hint,
               "WSS samples and calibration from another address are ignored while locked");
        otherHttp.abort();
        processFor(20);
    }
    const bool http = connectTls(openHttp, loopback, port);
    const auto allowed = http ? exchange(openHttp, "POST /calibrate HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
                              : QByteArray();
    expect(allowed.startsWith("HTTP/1.1 200 ") && engine.calibrationHint() != hint,
           "HTTPS calibration from the locked address succeeds");
    expect(connectTls(openWebSocket, loopback, port) && upgradeSocket(openWebSocket),
           "Keep WSS session open for server teardown");
    exchange(openWebSocket, clientFrame(0x81, R"({"ax":3,"grav":false,"src":"owner-wss"})"));
    expect(engine.imuSource() == QStringLiteral("owner-wss"), "WSS motion from the locked address succeeds");
    expect(exchange(openWebSocket, clientFrame(0x89, "ping")) == QByteArray::fromHex("8a04") + "ping",
           "WebSocket Pong echoes the Ping payload");

    const auto drainReply = [](QSslSocket &client, int timeoutMs) {
        QByteArray reply;
        client.setReadBufferSize(0);
        if (client.state() != QAbstractSocket::UnconnectedState) {
            QEventLoop drain;
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&client, &QSslSocket::readyRead, &drain, [&]() {
                reply += client.readAll();
            });
            QObject::connect(&client, &QSslSocket::disconnected, &drain, &QEventLoop::quit);
            QObject::connect(&timeout, &QTimer::timeout, &drain, &QEventLoop::quit);
            timeout.start(timeoutMs);
            reply += client.readAll();
            if (client.state() != QAbstractSocket::UnconnectedState) drain.exec();
        }
        reply += client.readAll();
        return reply;
    };

    // Hold the peer's receive window closed, then change a tail the server
    // cannot have streamed yet. A readAll()-based response keeps the old tail;
    // bounded chunking observes the change when it reaches that part of the file.
    QSslSocket apkClient;
    const bool apkConnected = connectTls(apkClient, loopback, port);
    if (apkConnected) {
        apkClient.setReadBufferSize(1);
        apkClient.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        apkClient.write("GET /steady-sender.apk HTTP/1.1\r\nHost: localhost\r\n\r\n");
        apkClient.flush();
        processFor(100);
    }
    QFile changed(apkPath);
    const bool tailChanged = changed.open(QIODevice::ReadWrite)
        && changed.seek(apkSize - changedTailSize)
        && changed.write(QByteArray(changedTailSize, 'C')) == changedTailSize
        && changed.flush();
    changed.close();
    apkClient.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
    const QByteArray apkReply = apkConnected ? drainReply(apkClient, 30000) : QByteArray();
    const qsizetype headerEnd = apkReply.indexOf("\r\n\r\n");
    const QByteArray apkBody = headerEnd >= 0 ? apkReply.mid(headerEnd + 4) : QByteArray();
    const bool apkStreamed = apkConnected && tailChanged && headerEnd >= 0 && apkBody.size() == apkSize
        && apkBody.endsWith(QByteArray(changedTailSize, 'C'));
    if (!apkStreamed) {
        std::cerr << "APK stream diagnostic: connected=" << apkConnected
                  << " tailChanged=" << tailChanged << " state=" << int(apkClient.state())
                  << " reply=" << apkReply.size() << " body=" << apkBody.size()
                  << " headerEnd=" << headerEnd << " clients=" << server->activeClientCount() << '\n';
    }
    expect(apkStreamed,
           "APK response streams bounded chunks instead of caching the whole file");
    apkClient.abort();
    processFor(20);

    // Once streaming reaches EOF, the TLS/socket queues may still contain the
    // body. Keep a small peer stalled beyond the old two-second forced-close
    // deadline, then release backpressure and require every byte.
    constexpr qint64 slowApkSize = 2 * 1024 * 1024;
    QFile slowApkFile(apkPath);
    const bool slowApkPrepared = slowApkFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && slowApkFile.resize(slowApkSize) && slowApkFile.seek(slowApkSize - changedTailSize)
        && slowApkFile.write(QByteArray(changedTailSize, 'D')) == changedTailSize
        && slowApkFile.flush();
    slowApkFile.close();
    QSslSocket slowApkClient;
    const bool slowApkConnected = slowApkPrepared && connectTls(slowApkClient, loopback, port);
    if (slowApkConnected) {
        slowApkClient.setReadBufferSize(1);
        slowApkClient.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        slowApkClient.write("GET /steady-sender.apk HTTP/1.1\r\nHost: localhost\r\n\r\n");
        slowApkClient.flush();
        processFor(5000);
        slowApkClient.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
    }
    const QByteArray slowApkReply = slowApkConnected ? drainReply(slowApkClient, 15000) : QByteArray();
    const qsizetype slowHeaderEnd = slowApkReply.indexOf("\r\n\r\n");
    const QByteArray slowApkBody = slowHeaderEnd >= 0 ? slowApkReply.mid(slowHeaderEnd + 4) : QByteArray();
    expect(slowApkConnected && slowHeaderEnd >= 0 && slowApkBody.size() == slowApkSize
               && slowApkBody.endsWith(QByteArray(changedTailSize, 'D')),
           "APK graceful shutdown drains queued bytes for a slow peer");
    slowApkClient.abort();
    processFor(20);

    // The large-transfer regressions can legitimately outlive the HTTP idle
    // window on sanitizer builds. Reopen both teardown fixtures immediately
    // before exercising the slow-peer cap and destructor.
    openHttp.abort();
    processFor(20);
    const bool teardownHttp = connectTls(openHttp, loopback, port);
    const QByteArray teardownHttpReply = teardownHttp
        ? exchange(openHttp, "POST /calibrate HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
        : QByteArray();
    expect(teardownHttp && teardownHttpReply.startsWith("HTTP/1.1 200 "),
           "Keep HTTP session open for server teardown");
    expect(exchange(openWebSocket, clientFrame(0x89, "live")) == QByteArray::fromHex("8a04") + "live",
           "Keep WSS session responsive for server teardown");

    // A bounded localhost-only regression for a peer whose TLS read window fills.
    QSslSocket slow;
    const bool upgraded = connectTls(slow, loopback, port) && upgradeSocket(slow);
    slow.setReadBufferSize(1);
    slow.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
    const auto pingBatch = clientFrame(0x89, QByteArray(125, 'p')).repeated(96);
    for (int i = 0; upgraded && i < 600 && server->activeClientCount() == 3; ++i) {
        if (slow.bytesToWrite() < pingBatch.size()) slow.write(pingBatch);
        processFor(2);
    }
    expect(upgraded && server->activeClientCount() == 2, "Slow WebSocket peer is dropped at the write queue cap");
    slow.abort();
    processFor(20);

    expect(server->activeClientCount() == 2, "Keep both HTTP and WSS clients alive during server destruction");
    server.reset();
    processFor(50);
    expect(openHttp.state() == QAbstractSocket::UnconnectedState &&
               openWebSocket.state() == QAbstractSocket::UnconnectedState,
           "Server destruction disconnects open TLS clients under ASan");
}

void testCertificateChildShutdown()
{
    QTemporaryDir cache;
    const QString marker = cache.filePath(QStringLiteral("openssl-child-running"));
    qputenv("STEADY_OPENSSL", QCoreApplication::applicationFilePath().toLocal8Bit());
    qputenv("STEADY_TEST_OPENSSL_MARKER", marker.toLocal8Bit());
    MotionEngine engine;
    SensorServer::Options options;
    options.httpsPort = 0;
    options.udpPort = 0;
    options.cacheDirectory = cache.path();
    auto server = std::make_unique<SensorServer>(&engine, options);
    QElapsedTimer wait;
    wait.start();
    while (!QFileInfo::exists(marker) && wait.elapsed() < 2500) processFor(10);
    expect(QFileInfo::exists(marker), "Certificate test child reaches its long-running branch");

    QStringList messages;
    g_capturedMessages = &messages;
    const QtMessageHandler previous = qInstallMessageHandler(captureMessages);
    QElapsedTimer teardown;
    teardown.start();
    server.reset();
    const qint64 elapsed = teardown.elapsed();
    qInstallMessageHandler(previous);
    g_capturedMessages = nullptr;
    bool destroyedWhileRunning = false;
    for (const QString &message : messages) {
        destroyedWhileRunning |= message.contains(QStringLiteral("Destroyed while process"));
    }
    expect(elapsed < 1500 && !destroyedWhileRunning,
           "Server teardown kills and reaps an in-flight certificate child without a QProcess warning");
    qunsetenv("STEADY_TEST_OPENSSL_MARKER");
    qunsetenv("STEADY_OPENSSL");
}

void testCli()
{
    const QString cli = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        QStringLiteral("../steady-cli.exe")
#else
        QStringLiteral("../steady-cli")
#endif
    );
    for (const QStringList &invalid : {QStringList{"run", "--mode", "bad"},
                                       QStringList{"run", "--udp-port", "65536"},
                                       QStringList{"send"}, QStringList{"discover", "--duration", "1"},
                                       QStringList{"unknown"}}) {
        QProcess process;
        process.start(cli, invalid);
        expect(finishProcess(process) && process.exitCode() == 2, "CLI rejects invalid arguments with exit code 2");
    }

    QUdpSocket reservation;
    expect(reservation.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DontShareAddress), "Reserve test UDP port");
    const QString port = QString::number(reservation.localPort());
    QProcess collision;
    collision.start(cli, {"run", "--no-https", "--udp-port", port});
    expect(finishProcess(collision, 2000) && collision.exitCode() == 3, "CLI reports occupied UDP port with exit code 3");
    reservation.close();

    {
        LocalControl blocker(QStringLiteral("receiver-") + port);
        expect(blocker.tryOwn(), "Reserve the CLI receiver's local-control lock");
        QProcess degraded;
        degraded.start(cli, {"run", "--no-https", "--json", "--udp-port", port,
                             "--duration", "0.25"});
        expect(finishProcess(degraded) && degraded.exitCode() == 0
                   && degraded.readAllStandardError().contains("commands unavailable"),
               "CLI continues when local-control setup is unavailable");
    }

    QProcess simulator;
    simulator.start(cli, {"run", "--json", "--no-https", "--mode", "sim",
                          "--udp-port", port, "--duration", "0.6"});
    const bool simExited = finishProcess(simulator);
    const auto simulated = telemetryLines(simulator.readAllStandardOutput());
    expect(simExited && simulator.exitCode() == 0 && simulated.size() >= 2
               && simulated.last().value("offsetX").toDouble() < -0.5,
           "CLI timed simulation exits 0 with ASCII JSON and moving dots");

    QTcpServer httpsPort;
    expect(httpsPort.listen(QHostAddress::AnyIPv4, 0), "Reserve test HTTPS port");
    const quint16 portNumber = httpsPort.serverPort();
    QProcess httpsCollision;
    httpsCollision.start(cli, {"run", "--json", "--udp-port", port, "--https-port",
                               QString::number(portNumber)});
    expect(finishProcess(httpsCollision) && httpsCollision.exitCode() == 3 &&
               httpsCollision.readAllStandardOutput().isEmpty(),
           "Occupied HTTPS port exits 3 without printing JSON telemetry");
    httpsPort.close();

    QProcess pair;
    pair.start(cli, {"pair", "--https-port", QString::number(portNumber), "--no-qr"});
    const bool paired = finishProcess(pair);
    const QByteArray pairing = pair.readAllStandardOutput();
    expect(paired && pair.exitCode() == 0 && pairing.contains("https://") &&
               pairing.contains(":" + QByteArray::number(portNumber)) && !pairing.contains("\xE2\x96"),
           "CLI pair prints URLs on the requested port without QR for piped output");
    QProcess receiver;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("STEADY_OPENSSL"), QStringLiteral(""));
    receiver.setProcessEnvironment(env);
    receiver.start(cli, {"run", "--json", "--udp-port", port, "--https-port", QString::number(portNumber),
                         "--rate", "10", "--duration", "2.6"});
    const bool receiverStarted = receiver.waitForReadyRead(2500);
    expect(receiverStarted, "CLI phone receiver prints telemetry while starting");

    QProcess discovery;
    discovery.start(cli, {"discover", "--port", port, "--timeout", "500"});
    QProcess sender;
    sender.start(cli, {"send", "--host", "127.0.0.1", "--port", port,
                       "--preset", "mountain", "--duration", "0.9"});
    QSslSocket unavailable;
    unavailable.setPeerVerifyMode(QSslSocket::VerifyNone);
    unavailable.connectToHostEncrypted(QStringLiteral("127.0.0.1"), portNumber);
    expect(!unavailable.waitForEncrypted(400), "Empty STEADY_OPENSSL leaves HTTPS unavailable");
    expect(finishProcess(sender) && sender.exitCode() == 0, "CLI sender exits 0");
    QProcess release;
    release.start(cli, {"release", "--port", port});
    expect(finishProcess(release) && release.exitCode() == 0 && release.readAllStandardError().isEmpty(),
           "CLI release reaches the running receiver");
    release.start(cli, {"release", "--port", port});
    expect(finishProcess(release) && release.exitCode() == 0 && release.readAllStandardError().contains("No source is locked"),
           "CLI release reports an empty source lock");
    const bool discoveryExited = finishProcess(discovery);
    bool foundReceiver = false;
    for (const auto &line : discovery.readAllStandardOutput().trimmed().split('\n')) {
        const auto reply = QJsonDocument::fromJson(line).object();
        foundReceiver |= reply.value("steady").toBool() && reply.value("udp").toInt() == port.toInt();
    }
    expect(discoveryExited && discovery.exitCode() == 0 && foundReceiver,
           "CLI discovery receives a reply from CLI receiver");

    const bool receiverExited = finishProcess(receiver);
    const auto received = telemetryLines(receiver.readAllStandardOutput());
    const auto status = receiver.readAllStandardError();
    expect(status.contains("openssl executable unavailable") && status.contains("ingest and discovery ready"),
           "Empty STEADY_OPENSSL reports HTTPS off and UDP ready");
    bool packetChangedTelemetry = false;
    for (const auto &row : received) {
        if (row.value("connected").toBool() && row.value("source") == QStringLiteral("Steady CLI")
            && std::abs(row.value("ax").toDouble()) > 0.3 && std::abs(row.value("offsetX").toDouble()) > 0.5) {
            packetChangedTelemetry = true;
        }
    }
    expect(receiverExited && receiver.exitCode() == 0 && packetChangedTelemetry,
           "CLI UDP round-trip connects the receiver and changes acceleration and offset");

    receiver.start(cli, {"run", "--no-https", "--no-source-lock", "--json", "--udp-port", port, "--duration", "0.9"});
    expect(receiver.waitForReadyRead(2000), "CLI receiver starts with the lock disabled");
    QUdpSocket unlockedSender;
    unlockedSender.writeDatagram(R"({"ax":2,"grav":false,"src":"unlocked"})", QHostAddress::LocalHost, port.toUShort());
    expect(finishProcess(receiver), "Lock-disabled receiver exits cleanly");
    const auto unlocked = telemetryLines(receiver.readAllStandardOutput());
    expect(!unlocked.isEmpty() && unlocked.last().value("source").toString() == QStringLiteral("unlocked") &&
               unlocked.last().value("lockedSource").toString().isEmpty(),
           "CLI --no-source-lock accepts motion without claiming a source");
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("steady-test"));

    if (argc > 1 && QByteArray(argv[1]) == "req"
        && qEnvironmentVariableIsSet("STEADY_TEST_OPENSSL_MARKER")) {
        QFile marker(qEnvironmentVariable("STEADY_TEST_OPENSSL_MARKER"));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("ready") != 5 || !marker.flush()) return 4;
        marker.close();
        return app.exec();
    }
    if (argc == 5 && QByteArray(argv[1]) == "--local-control-stale-child") {
        LocalControl control(QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[2]));
        if (!control.tryOwn() || !control.listen("p", [](char) { return LocalControl::Reply::Accepted; })) return 5;
        QFile marker(QString::fromLocal8Bit(argv[4]));
        if (!marker.open(QIODevice::WriteOnly) || marker.write("ready") != 5 || !marker.flush()) return 6;
        marker.close();
        std::_Exit(0);
    }

    const QrMatrix qr = encodeQr(QStringLiteral("HELLO WORLD"));
    const auto hash = QCryptographicHash::hash(qr.modules, QCryptographicHash::Sha256).toHex();
    // Independently generated with python-qrcode, Model 2 version 1, ECC Q, mask 0.
    expect(qr.size == 21 && hash == "e5395d7a0fb74b1e1b0b752cbb72ee155a5ad7b4cbd2023fe17384aa002230b1",
           "QR matrix matches the version 1/Q HELLO WORLD reference");
    expect(AppPaths::cacheDirectory() == QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               && AppPaths::dataDirectory() == QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
               && AppPaths::configDirectory() == QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
           "Cache, data and config paths use Qt platform locations");
    const QString root = QDir::tempPath() + QStringLiteral("/steady-paths");
    QStringList apk{root + "/bin/steady-sender.apk", root + "/share/steady/steady-sender.apk",
                    root + "/user/steady-sender.apk"};
#ifdef Q_OS_LINUX
    apk.append(QStringLiteral("/usr/share/steady/steady-sender.apk"));
#endif
    expect(AppPaths::apkCandidates(root + "/bin", root + "/user") == apk,
           "APK lookup follows executable, relative share, user data, then Linux system share");

    // 1. MotionEngine: NaN/Infinity rejection & magnitude clamping
    {
        MotionEngine eng(MotionEngine::Timing::Manual);
        eng.setMode(QStringLiteral("phone"));
        eng.ingestImu(std::nan(""), 1.0, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        eng.ingestImu(1.0, INFINITY, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        expect(!eng.phoneConnected(), "NaN/Inf IMU packets are rejected before updating state");

        eng.ingestImu(999.0, -999.0, 0.0, 0.0, 0.0, 999.0, false, QStringLiteral("Test"));
        eng.advance(0.080);
        expect(eng.phoneConnected(), "Valid clamped IMU packet connects phone");
        expect(std::isfinite(eng.ax()) && std::isfinite(eng.ay()) && std::isfinite(eng.offsetX()),
               "Clamped extreme IMU values keep filter & kinematics finite");
    }

    // 2. MotionEngine: calibrate() idempotency & gravity reset
    {
        MotionEngine eng(MotionEngine::Timing::Manual);
        eng.setMode(QStringLiteral("phone"));
        eng.ingestImu(2.0, -1.5, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        eng.calibrate();
        eng.calibrate(); // second click before next packet must not double-add bias
        eng.ingestImu(2.0, -1.5, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        eng.advance(0.080);
        expect(std::abs(eng.ax()) < 0.05 && std::abs(eng.ay()) < 0.05,
               "Repeated calibrate() is idempotent and zeroes linear offset");

        // gravityIncluded mode should not accumulate false bias
        eng.ingestImu(0.5, 0.5, 9.8, 0.0, 0.0, 0.0, true, QStringLiteral("Test"));
        eng.calibrate();
        eng.ingestImu(0.5, 0.5, 9.8, 0.0, 0.0, 0.0, true, QStringLiteral("Test"));
        eng.advance(0.080);
        expect(std::abs(eng.ax()) < 0.05 && std::abs(eng.ay()) < 0.05,
               "calibrate() with gravityIncluded resets gravity cleanly");
    }

    // 3. MotionEngine: idle-sleep stops timer when settled (0 Hz CPU wakeups)
    {
        MotionEngine eng(MotionEngine::Timing::Manual);
        eng.setAutoHide(false);
        eng.setMode(QStringLiteral("sim"));
        eng.setSimPreset(QStringLiteral("manual"));
        eng.triggerImpulse(2.0, 0.0);
        expect(eng.isAnimationTimerActive(), "Animation timer wakes on impulse");
        eng.advance(3.2);
        expect(!eng.isAnimationTimerActive(), "Animation timer stops automatically after settling (idle-sleep)");
        eng.triggerImpulse(-1.5, 0.0);
        expect(eng.isAnimationTimerActive(), "Animation timer wakes immediately on new motion");
    }

    {
        MotionEngine engine(MotionEngine::Timing::Manual);
        engine.setMode(QStringLiteral("sim"));
        engine.setSimPreset(QStringLiteral("manual"));
        engine.advance(1.5);
        expect(!engine.isAnimationTimerActive() && engine.currentAlpha() == 1.0,
               "Settled kinematics stop the frame timer before the fade starts");
        engine.setSimPreset(QStringLiteral("bogus"));
        expect(!engine.isAnimationTimerActive() && engine.simPreset() == QLatin1String("manual"),
               "Unknown preset leaves a valid idle selection idle");
        engine.advance(3.0);
        expect(engine.isAnimationTimerActive() && engine.currentAlpha() < 1.0, "Idle deadline starts fade");
        engine.advance(4.5);
        expect(!engine.isAnimationTimerActive() && engine.currentAlpha() == 0.0, "Fade ends at zero and animation stops");
    }

    // 4. MotionEngine: disconnect watchdog still runs when overlayEnabled == false
    {
        MotionEngine eng(MotionEngine::Timing::Manual);
        eng.setOverlayEnabled(false);
        eng.ingestImu(0.2, 0.0, 0.0, 0.0, 0.0, 0.0, false, QStringLiteral("Test"));
        expect(eng.phoneConnected(), "Phone connects even while overlay disabled");
        eng.advance(3.499);
        expect(eng.phoneConnected(), "Watchdog keeps a timestamp-zero packet alive just before 3.5 s");
        eng.advance(0.001);
        expect(!eng.phoneConnected(), "Disconnect watchdog clears phoneConnected even when overlayEnabled == false");
    }

    // 5. SensorServer: UDP discovery + NaN CSV rejection + HTTPS/WS overflow rejection
    {
        MotionEngine eng;
        eng.setMode(QStringLiteral("phone"));
        QTemporaryDir cache;
        const QString staleDirectory = QDir(cache.path()).filePath(QStringLiteral("tls-stale-test"));
        QDir().mkdir(staleDirectory);
        QFile staleConfig(QDir(staleDirectory).filePath(QStringLiteral("openssl.cnf")));
        const bool stalePrepared = staleConfig.open(QIODevice::ReadWrite)
            && staleConfig.write("stale") == 5 && staleConfig.flush()
            && staleConfig.setFileTime(QDateTime::currentDateTimeUtc().addDays(-2), QFileDevice::FileModificationTime);
        staleConfig.close();
        expect(stalePrepared, "Prepare an old abandoned TLS staging directory");
        SensorServer::Options options;
        options.httpsPort = 0;
        options.udpPort = 0;
        options.cacheDirectory = cache.path();
        SensorServer srv(&eng, options);
        waitForHttps(srv);
        expect(!QFileInfo::exists(staleDirectory), "HTTPS startup removes abandoned TLS staging directories");
        std::cout << srv.statusMessage().toUtf8().constData() << '\n';
        expect(srv.httpsReady() && srv.httpsPort() > 0 && srv.udpPort() > 0,
               "SensorServer binds ephemeral HTTPS and UDP ports for testing");

        {
            QTcpServer occupied;
            expect(occupied.listen(QHostAddress::AnyIPv4, 0), "Reserve HTTPS port for bind failure test");
            SensorServer::Options conflictOptions = options;
            conflictOptions.httpsPort = occupied.serverPort();
            SensorServer conflict(&eng, conflictOptions);
            waitForHttps(conflict);
            expect(conflict.httpsBindFailed() && !conflict.httpsReady() && conflict.udpReady()
                       && conflict.lanUrl().isEmpty() && conflict.tailscaleUrl().isEmpty()
                       && conflict.activeQr().size == 0 && conflict.activeQrUrl().isEmpty()
                       && conflict.statusMessage().contains("cannot bind") && conflict.statusMessage().contains("discovery ready"),
                   "Occupied HTTPS port reports failure and suppresses URL and QR while UDP stays up");

            conflictOptions.udpPort = srv.udpPort();
            conflictOptions.httpsEnabled = false;
            SensorServer udpConflict(&eng, conflictOptions);
            expect(!udpConflict.udpReady() && udpConflict.statusMessage().contains("unavailable"),
                   "Occupied UDP port is reported honestly");
        }

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
            expect(obj.value(QStringLiteral("lan")).toString() == QStringLiteral("127.0.0.1"),
                   "Discovery advertises the unicast address the request reached");
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
        testTlsRegressions(options);
    }

    testUdpBurst();
    testUdpParsing();
    testDiscoveryRateLimit();
    testLocalControl();
    testSourceLock();
    testGravityResponse();
    testIdleKeepalive();
    testSettings();
    testSimulationTimeout();
    testCertificateChildShutdown();
    testCli();

    if (g_failures > 0) {
        std::cerr << "\nFAILED: " << g_failures << " assertion(s)\n";
        return 1;
    }
    std::cout << "\nALL TESTS PASSED\n";
    return 0;
}
