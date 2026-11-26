#include "local_control.h"
#include "motion_engine.h"
#include "sensor_server.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QTextStream>

#include <cmath>
#include <cstdio>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {
#ifdef Q_OS_WIN
volatile LONG interrupted = 0;
BOOL WINAPI onConsoleSignal(DWORD signal)
{
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}

class Terminal {
public:
    Terminal()
    {
        if (!isTerminal) return;
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleMode(m_output, m_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    ~Terminal()
    {
        if (!isTerminal) return;
        SetConsoleMode(m_output, m_mode);
        if (m_codePage) SetConsoleOutputCP(m_codePage);
    }

private:
    const HANDLE m_output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m_mode = 0;
    const UINT m_codePage = GetConsoleOutputCP();

public:
    const bool isTerminal = GetConsoleMode(m_output, &m_mode) != 0;
};
#else
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int)
{
    interrupted = 1;
}

struct Terminal {
    const bool isTerminal = isatty(fileno(stdout)) != 0;
};
#endif

int usage(const QString &reason)
{
    QTextStream(stderr) << "steady-cli: " << reason << "\nSee steady-cli --help.\n";
    return 2;
}

struct Arguments {
    QString command;
    QString mode;
    QString preset;
    QString host;
    QHostAddress bindAddress = QHostAddress::AnyIPv4;
    quint16 udpPort = 8766;
    quint16 httpsPort = 8765;
    int rate = 4;
    int sendHz = 25;
    int timeoutMs = 1000;
    int durationMs = -1;
    bool noHttps = false;
    bool noQr = false;
    bool noSourceLock = false;
    bool json = false;
};

bool parseArguments(const QCoreApplication &app, Arguments &args, QString &error)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Receive phone IMU or simulate motion (run, default); print pairing URLs (pair);\n"
        "broadcast discovery (discover); send simulated IMU (send); or release a receiver's source (release)."));
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("run | pair | discover | send | release"),
                                 QStringLiteral("[run|pair|discover|send|release]"));
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    using Option = QCommandLineOption;
    const Option mode("mode", "Run mode: sim or phone.", "mode", "phone");
    const Option preset("preset", "Simulation: city, highway, mountain or manual.", "preset", "city");
    const Option udpPort("udp-port", "UDP listen port for run.", "port", "8766");
    const Option httpsPort("https-port", "HTTPS port for run or pair.", "port", "8765");
    const Option noHttps("no-https", "Disable HTTPS for run.");
    const Option noSourceLock("no-source-lock", "Accept motion from multiple addresses.");
    const Option noQr("no-qr", "Suppress terminal QR codes.");
    const Option json("json", "Print only JSON telemetry on standard output.");
    const Option rate("rate", "Telemetry lines per second.", "Hz", "4");
    const Option duration("duration", "Exit after this many seconds (run or send).", "seconds");
    const Option timeout("timeout", "Discovery timeout in milliseconds.", "ms", "1000");
    const Option host("host", "Destination hostname or IPv4 address for send.", "host");
    const Option port("port", "Receiver UDP port for discover, send or release.", "port", "8766");
    const Option bind("bind", "Local IPv4 source address for send.", "address");
    const Option hz("hz", "Packets per second for send.", "Hz", "25");
    parser.addOptions({mode, preset, udpPort, httpsPort, noHttps, noQr, json, rate, duration,
                       timeout, host, port, bind, hz, noSourceLock});
    if (!parser.parse(app.arguments())) {
        error = parser.errorText();
        return false;
    }
    if (parser.isSet(help)) parser.showHelp(0);
    if (parser.isSet(version)) parser.showVersion();

    const auto commands = parser.positionalArguments();
    args.command = commands.isEmpty() ? QStringLiteral("run") : commands.first();
    QList<Option> accepted;
    if (args.command == QLatin1String("run")) {
        accepted = {mode, preset, udpPort, httpsPort, noHttps, noQr, json, rate, duration, noSourceLock};
    } else if (args.command == QLatin1String("pair")) {
        accepted = {httpsPort, noQr};
    } else if (args.command == QLatin1String("discover")) {
        accepted = {timeout, port};
    } else if (args.command == QLatin1String("send")) {
        accepted = {host, port, preset, hz, duration, bind};
    } else if (args.command == QLatin1String("release")) {
        accepted = {port};
    }
    if (accepted.isEmpty() || commands.size() > 1) {
        error = QStringLiteral("expected one command: run, pair, discover, send or release");
        return false;
    }
    QSet<QString> allowed;
    for (const auto &opt : accepted) allowed.insert(opt.names().first());
    for (const QString &name : parser.optionNames()) {
        if (!allowed.contains(name)) {
            error = QStringLiteral("--%1 is not valid for %2").arg(name, args.command);
            return false;
        }
    }

    const auto number = [&](const Option &opt, int max) {
        bool ok;
        const int n = parser.value(opt).toInt(&ok);
        if (!ok || n < 1 || n > max) {
            error = QStringLiteral("--%1 must be an integer from 1 to %2").arg(opt.names().first()).arg(max);
            return 0;
        }
        return n;
    };
    args.mode = parser.value(mode);
    args.preset = parser.value(preset);
    args.host = parser.value(host);
    args.udpPort = quint16(number(args.command == QLatin1String("run") ? udpPort : port, 65535));
    args.httpsPort = quint16(number(httpsPort, 65535));
    args.rate = number(rate, 1000);
    args.sendHz = number(hz, 1000);
    args.timeoutMs = number(timeout, std::numeric_limits<int>::max());
    args.noHttps = parser.isSet(noHttps);
    args.noQr = parser.isSet(noQr);
    args.noSourceLock = parser.isSet(noSourceLock);
    args.json = parser.isSet(json);
    if (args.mode != QLatin1String("sim") && args.mode != QLatin1String("phone")) {
        error = QStringLiteral("--mode must be sim or phone");
    } else if (!MotionEngine::validPreset(args.preset)) {
        error = QStringLiteral("unknown --preset");
    } else if (args.command == QLatin1String("send") && args.host.trimmed().isEmpty()) {
        error = QStringLiteral("send requires --host");
    } else if (parser.isSet(bind) && (!args.bindAddress.setAddress(parser.value(bind))
              || args.bindAddress.protocol() != QAbstractSocket::IPv4Protocol)) {
        error = QStringLiteral("--bind must be a local IPv4 address");
    }
    if (parser.isSet(duration)) {
        bool ok;
        const double ms = parser.value(duration).toDouble(&ok) * 1000.0;
        if (!ok || !std::isfinite(ms) || ms < 0 || ms > std::numeric_limits<int>::max()) {
            error = QStringLiteral("--duration must be a nonnegative number of seconds within the timer range");
        } else {
            args.durationMs = qRound(ms);
        }
    }
    return error.isEmpty();
}

void printQr(QTextStream &out, const QrMatrix &qr)
{
    if (qr.size == 0) return;
    constexpr int quiet = QrMatrix::quietZone;
    const bool color = qEnvironmentVariable("TERM") != QLatin1String("dumb");
    for (int y = -quiet; y < qr.size + quiet; y += 2) {
        if (color) out << "\033[30;47m";
        for (int x = -quiet; x < qr.size + quiet; ++x) {
            const bool top = qr.isDark(x, y);
            const bool bottom = qr.isDark(x, y + 1);
            if (top && bottom) out << QChar(0x2588);
            else if (top) out << QChar(0x2580);
            else if (bottom) out << QChar(0x2584);
            else out << ' ';
        }
        if (color) out << "\033[0m";
        out << '\n';
    }
}

void printPairing(QTextStream &out, const QString &lan, const QString &tailscale, bool qr)
{
    if (!lan.isEmpty()) out << lan << '\n';
    if (!tailscale.isEmpty()) out << tailscale << '\n';
    if (qr) printQr(out, encodeQr(lan.isEmpty() ? tailscale : lan));
    out.flush();
}

QString asciiJson(const QJsonObject &object)
{
    const QString json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QString ascii;
    ascii.reserve(json.size());
    for (QChar ch : json) {
        if (ch.unicode() < 128) ascii.append(ch);
        else ascii.append(QStringLiteral("\\u%1").arg(unsigned(ch.unicode()), 4, 16, QLatin1Char('0')));
    }
    return ascii;
}

QString receiverScope(quint16 udpPort)
{
    return QStringLiteral("receiver-%1").arg(udpPort);
}

int run(const Arguments &args, QTextStream &out, QTextStream &err, bool showQr)
{
    MotionEngine engine(MotionEngine::Timing::Realtime, MotionEngine::SimulationTimeout::Disabled);
    engine.setMode(args.mode);
    engine.setSimPreset(args.preset);
    SensorServer::Options options;
    options.udpPort = args.udpPort;
    options.httpsPort = args.httpsPort;
    options.httpsEnabled = !args.noHttps;
    options.sourceLockEnabled = !args.noSourceLock;
    SensorServer server(&engine, options);
    if (!server.udpReady()) {
        err << server.statusMessage() << Qt::endl;
        return 3;
    }
    LocalControl control(receiverScope(server.udpPort()));
    if (!control.tryOwn() || !control.listen("r", [&](char) {
        return server.releaseSource() ? LocalControl::Reply::Accepted : LocalControl::Reply::NoSource;
    })) {
        err << "Local receiver commands unavailable; continuing without them." << Qt::endl;
    }

    bool reported = false;
    QObject::connect(&server, &SensorServer::endpointsChanged, &server, [&]() {
        if (reported || server.httpsStarting()) return;
        reported = true;
        err << server.statusMessage() << Qt::endl;
        if (server.httpsBindFailed()) {
            QCoreApplication::exit(3);
        } else if (!args.json) {
            if (server.httpsReady()) {
                QString lan = server.lanUrl();
                if (lan.isEmpty() && server.tailscaleUrl().isEmpty()) {
                    lan = SensorServer::pairingUrl(QStringLiteral("127.0.0.1"), server.httpsPort());
                }
                printPairing(out, lan, server.tailscaleUrl(), showQr);
            } else {
                const auto udpUrl = [&](const QString &ip) {
                    return ip.isEmpty() ? QString() : QStringLiteral("udp://%1:%2").arg(ip).arg(server.udpPort());
                };
                const QString lan = server.lanIp().isEmpty() && server.tailscaleIp().isEmpty()
                    ? QStringLiteral("127.0.0.1") : server.lanIp();
                printPairing(out, udpUrl(lan), udpUrl(server.tailscaleIp()), false);
            }
        }
    });

    QElapsedTimer clock;
    clock.start();
    QTimer telemetry;
    telemetry.setTimerType(Qt::PreciseTimer);
    const auto print = [&]() {
        if (!reported || server.httpsBindFailed()) return;
        const double seconds = double(clock.elapsed()) / 1000.0;
        if (args.json) {
            out << asciiJson({
                {"t", seconds}, {"ax", engine.ax()}, {"ay", engine.ay()},
                {"offsetX", engine.offsetX()}, {"offsetY", engine.offsetY()},
                {"alpha", engine.currentAlpha()}, {"hz", engine.imuHz()},
                {"connected", engine.phoneConnected()}, {"source", engine.imuSource()},
                {"lockedSource", server.lockedSource()}, {"animationActive", engine.isAnimationTimerActive()}
            }) << '\n';
        } else {
            out << "t=" << seconds << " ax=" << engine.ax() << " ay=" << engine.ay()
                << " offsetX=" << engine.offsetX() << " offsetY=" << engine.offsetY()
                << " alpha=" << engine.currentAlpha() << " Hz=" << engine.imuHz()
                << " connected=" << (engine.phoneConnected() ? "yes" : "no")
                << " lockedSource=" << server.lockedSource()
                << " source=" << engine.imuSource() << '\n';
        }
        out.flush();
    };
    QObject::connect(&telemetry, &QTimer::timeout, &engine, print);
    QTimer::singleShot(0, &engine, print);
    telemetry.start(qRound(1000.0 / args.rate));
    if (args.durationMs >= 0) QTimer::singleShot(args.durationMs, &engine, &QCoreApplication::quit);
    return QCoreApplication::exec();
}

int discover(const Arguments &args, QTextStream &out, QTextStream &err)
{
    QUdpSocket udp;
    if (!udp.bind(QHostAddress::AnyIPv4, 0)) {
        err << "Cannot bind discovery socket: " << udp.errorString() << Qt::endl;
        return 3;
    }

    const QByteArray query(R"({"cmd":"discover"})");
    QSet<QHostAddress> destinations = {QHostAddress::Broadcast, QHostAddress::LocalHost};
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        for (const auto &entry : iface.addressEntries()) {
            const auto broadcast = entry.broadcast();
            if (broadcast.protocol() == QAbstractSocket::IPv4Protocol) destinations.insert(broadcast);
        }
    }
    for (const auto &address : destinations) udp.writeDatagram(query, address, args.udpPort);

    QSet<QByteArray> seen;
    QObject::connect(&udp, &QUdpSocket::readyRead, &udp, [&]() {
        while (udp.hasPendingDatagrams()) {
            const auto reply = udp.receiveDatagram(4096);
            const auto object = QJsonDocument::fromJson(reply.data()).object();
            if (object.value(QStringLiteral("steady")).toBool() && !seen.contains(reply.data())) {
                seen.insert(reply.data());
                out << asciiJson(object) << Qt::endl;
            }
        }
    });
    QTimer::singleShot(args.timeoutMs, &udp, &QCoreApplication::quit);
    return QCoreApplication::exec();
}

int send(const Arguments &args, QTextStream &err)
{
    QUdpSocket udp;
    if (!udp.bind(args.bindAddress, 0)) {
        err << "Cannot bind sender socket: " << udp.errorString() << Qt::endl;
        return 3;
    }

    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    QHostAddress address;
    double seconds = 0;
    const auto packet = [&]() {
        seconds += 1.0 / args.sendHz;
        const auto sample = MotionEngine::simulationSample(args.preset, seconds);
        const QJsonObject object{
            {"ax", sample.x}, {"ay", sample.y}, {"az", 0}, {"grav", false}, {"src", "Steady CLI"}
        };
        if (udp.writeDatagram(QJsonDocument(object).toJson(QJsonDocument::Compact), address, args.udpPort) < 0) {
            err << "Cannot send UDP packet: " << udp.errorString() << Qt::endl;
            QCoreApplication::exit(3);
        }
    };
    QObject::connect(&timer, &QTimer::timeout, &udp, packet);
    QHostInfo::lookupHost(args.host, &udp, [&](const QHostInfo &host) {
        for (const auto &candidate : host.addresses()) {
            if (candidate.protocol() != QAbstractSocket::IPv4Protocol) continue;
            address = candidate;
            break;
        }
        if (address.isNull()) {
            QCoreApplication::exit(usage(QStringLiteral("cannot resolve --host to an IPv4 address")));
            return;
        }
        packet();
        timer.start(qRound(1000.0 / args.sendHz));
        if (args.durationMs >= 0) QTimer::singleShot(args.durationMs, &udp, &QCoreApplication::quit);
    });
    return QCoreApplication::exec();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("steady-cli"));
    app.setApplicationVersion(QStringLiteral(STEADY_VERSION));
    Arguments args;
    QString error;
    if (!parseArguments(app, args, error)) return usage(error);
    // Share cache and data locations with the desktop app.
    app.setApplicationName(QStringLiteral("steady"));

    const Terminal terminal;
    QTextStream out(stdout), err(stderr);
    const bool showQr = terminal.isTerminal && !args.noQr;
    if (args.command == QLatin1String("pair")) {
        auto addresses = SensorServer::networkAddresses();
        if (addresses.lan.isEmpty() && addresses.tailscale.isEmpty()) addresses.lan = QStringLiteral("127.0.0.1");
        printPairing(out, SensorServer::pairingUrl(addresses.lan, args.httpsPort),
                     SensorServer::pairingUrl(addresses.tailscale, args.httpsPort), showQr);
        return 0;
    }
    if (args.command == QLatin1String("release")) {
        LocalControl control(receiverScope(args.udpPort));
        const auto reply = control.send('r');
        if (reply == LocalControl::Reply::Accepted) return 0;
        if (reply == LocalControl::Reply::NoSource) {
            err << "No source is locked on UDP port " << args.udpPort << Qt::endl;
            return 0;
        }
        err << "Cannot reach the local steady-cli receiver on UDP port " << args.udpPort << Qt::endl;
        return 3;
    }

#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(onConsoleSignal, TRUE);
#else
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#endif
    QTimer signalCheck;
    QObject::connect(&signalCheck, &QTimer::timeout, &app, [&app]() {
#ifdef Q_OS_WIN
        if (InterlockedCompareExchange(&interrupted, 0, 0)) app.quit();
#else
        if (interrupted) app.quit();
#endif
    });
    signalCheck.start(50);

    if (args.command == QLatin1String("discover")) return discover(args, out, err);
    if (args.command == QLatin1String("send")) return send(args, err);
    return run(args, out, err, showQr);
}
