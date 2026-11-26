#include "app_paths.h"
#include "local_control.h"
#include "motion_engine.h"
#include "overlay_window.h"
#include "sensor_server.h"
#include "theme.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QThread>
#include <memory>

namespace {
class PanelInteractionFilter : public QObject {
public:
    explicit PanelInteractionFilter(MotionEngine &engine) : m_engine(engine) {}

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        const auto type = event->type();
        if (type == QEvent::KeyPress || type == QEvent::MouseButtonPress || type == QEvent::Wheel
            || type == QEvent::TouchBegin || type == QEvent::TouchUpdate
            || (type == QEvent::MouseMove && static_cast<QMouseEvent *>(event)->buttons())) {
            m_engine.noteInteraction();
        }
        return false;
    }

private:
    MotionEngine &m_engine;
};

class PairingImageProvider : public QQuickImageProvider {
public:
    explicit PairingImageProvider(const SensorServer &server)
        : QQuickImageProvider(QQuickImageProvider::Image), m_server(server) {}

    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        const QrMatrix qr = m_server.activeQr();
        if (qr.size == 0) return {};
        const int side = qr.size + 2 * QrMatrix::quietZone;
        QImage image(side, side, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < qr.size; ++y) {
            for (int x = 0; x < qr.size; ++x) {
                if (qr.isDark(x, y)) {
                    image.setPixel(x + QrMatrix::quietZone, y + QrMatrix::quietZone, qRgb(0, 0, 0));
                }
            }
        }
        if (size) *size = image.size();
        return image;
    }

private:
    const SensorServer &m_server;
};

class ControlPanel : public QObject {
public:
    ControlPanel(Theme &theme, MotionEngine &motion, SensorServer &server)
        : m_theme(theme), m_motion(motion), m_server(server)
    {
        QQuickStyle::setStyle(QStringLiteral("Material"));
    }

    bool show()
    {
        if (!m_qml) {
            m_qml = std::make_unique<QQmlApplicationEngine>();
            m_qml->addImageProvider(QStringLiteral("pairing"), new PairingImageProvider(m_server));
            m_qml->rootContext()->setContextProperty(QStringLiteral("theme"), &m_theme);
            m_qml->rootContext()->setContextProperty(QStringLiteral("motion"), &m_motion);
            m_qml->rootContext()->setContextProperty(QStringLiteral("server"), &m_server);
            m_qml->load(QUrl(QStringLiteral("qrc:/Main.qml")));
            if (m_qml->rootObjects().isEmpty()) {
                m_qml.reset();
                return false;
            }
            m_window = qobject_cast<QQuickWindow *>(m_qml->rootObjects().first());
            if (!m_window) {
                m_qml.reset();
                return false;
            }
            connect(m_window, &QWindow::visibleChanged, this, [this](bool visible) {
                if (visible) return;
                QTimer::singleShot(0, this, [this]() {
                    if (!m_window || m_window->isVisible()) return;
                    m_window = nullptr;
                    m_qml.reset();
                    if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS"))
                        qInfo("control panel unloaded");
                });
            });
        }
        if (m_window->visibility() == QWindow::Minimized) m_window->showNormal();
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
        if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS")) qInfo("control panel shown");
        return true;
    }

private:
    Theme &m_theme;
    MotionEngine &m_motion;
    SensorServer &m_server;
    std::unique_ptr<QQmlApplicationEngine> m_qml;
    QQuickWindow *m_window = nullptr;
};
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("steady"));
    app.setApplicationVersion(QStringLiteral(STEADY_VERSION));
    app.setDesktopFileName(QStringLiteral("steady"));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption httpsPort(QStringLiteral("https-port"), QStringLiteral("HTTPS listen port."),
                                     QStringLiteral("port"), QStringLiteral("8765"));
    const QCommandLineOption udpPort(QStringLiteral("udp-port"), QStringLiteral("UDP listen port."),
                                   QStringLiteral("port"), QStringLiteral("8766"));
    const QCommandLineOption noHttps(QStringLiteral("no-https"), QStringLiteral("Disable HTTPS pairing."));
    const QCommandLineOption mode(QStringLiteral("mode"), QStringLiteral("Startup mode: sim or phone."),
                                 QStringLiteral("mode"));
    const QCommandLineOption preset(QStringLiteral("preset"), QStringLiteral("Startup simulation preset."),
                                   QStringLiteral("preset"));
    const QCommandLineOption noPanel(QStringLiteral("no-panel"), QStringLiteral("Start without the control panel."));
    const QCommandLineOption quit(QStringLiteral("quit"), QStringLiteral("Quit a running Steady instance."));
    const QCommandLineOption noSourceLock(QStringLiteral("no-source-lock"), QStringLiteral("Accept motion from multiple addresses."));
    parser.addOptions({httpsPort, udpPort, noHttps, mode, preset, noPanel, quit, noSourceLock});
    parser.process(app);
    bool httpsPortValid, udpPortValid;
    SensorServer::Options options;
    options.httpsPort = parser.value(httpsPort).toUShort(&httpsPortValid);
    options.udpPort = parser.value(udpPort).toUShort(&udpPortValid);
    options.httpsEnabled = !parser.isSet(noHttps);
    const QString startMode = parser.value(mode);
    const QString startPreset = parser.value(preset);
    if (!httpsPortValid || !udpPortValid || !parser.positionalArguments().isEmpty()
        || (parser.isSet(mode) && startMode != QLatin1String("sim") && startMode != QLatin1String("phone"))
        || (parser.isSet(preset) && !MotionEngine::validPreset(startPreset))) {
        parser.showHelp(2);
    }

    LocalControl control(QStringLiteral("desktop"));
    const char command = parser.isSet(quit) ? 'q' : parser.isSet(noPanel) ? 'n' : 'p';
    QElapsedTimer startup;
    startup.start();
    bool ownsControl = control.tryOwn();
    while (!ownsControl && control.mayHaveOwner()) {
        const auto reply = control.send(command);
        if (reply == LocalControl::Reply::Accepted) return 0;
        if (reply == LocalControl::Reply::Failed || startup.elapsed() >= 3000) {
            qWarning("Could not reach the running Steady instance");
            break;
        }
        QThread::msleep(25);
        ownsControl = control.tryOwn();
    }
    if (command == 'q') return ownsControl ? 0 : 1;

    Theme theme;
    MotionEngine engine;
    QSettings settings(QDir(AppPaths::configDirectory()).filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
    engine.loadSettings(settings);
    if (parser.isSet(mode)) engine.setMode(startMode);
    if (parser.isSet(preset)) engine.setSimPreset(startPreset);
    if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS")) {
        qInfo("startup mode=%s animation=%d", qPrintable(engine.mode()), engine.isAnimationTimerActive());
    }
    PanelInteractionFilter interaction(engine);
    app.installEventFilter(&interaction);
    const auto save = [&]() { engine.saveSettings(settings); };
    QObject::connect(&engine, &MotionEngine::modeChanged, &settings, save);
    QObject::connect(&engine, &MotionEngine::simPresetChanged, &settings, save);
    QObject::connect(&engine, &MotionEngine::visualsChanged, &settings, save);
    const QString sourceLockKey = QStringLiteral("sourceLock");
    options.sourceLockEnabled = !parser.isSet(noSourceLock) && settings.value(sourceLockKey, true).toBool();
    SensorServer server(&engine, options);
    QObject::connect(&server, &SensorServer::sourceLockEnabledChanged, &settings, [&]() {
        settings.setValue(sourceLockKey, server.sourceLockEnabled());
    });
    OverlayStrip leftStrip(OverlayStrip::Side::Left, &engine, &theme);
    OverlayStrip rightStrip(OverlayStrip::Side::Right, &engine, &theme);

    ControlPanel panel(theme, engine, server);
    if (!ownsControl || !control.listen("pnq", [&](char request) {
        if (request == 'q') app.quit();
        if (request == 'p' && !panel.show()) qWarning("Could not load the Steady control panel");
        return LocalControl::Reply::Accepted;
    })) {
        qWarning("Single-instance control unavailable; running without it");
    }

    leftStrip.syncVisibility();
    rightStrip.syncVisibility();
    if (command == 'p' && !panel.show()) return 1;
#ifdef Q_OS_MACOS
    // Install after startup so the initial activation respects --no-panel.
    QTimer::singleShot(0, &panel, [&]() {
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &panel, [&](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !panel.show())
                qWarning("Could not load the Steady control panel");
        });
    });
#endif
    return app.exec();
}
