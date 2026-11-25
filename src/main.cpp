#include "motion_engine.h"
#include "overlay_window.h"
#include "sensor_server.h"
#include "theme.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("steady"));
    app.setDesktopFileName(QStringLiteral("steady"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    Theme theme;
    MotionEngine engine;
    SensorServer server(&engine);
    OverlayStrip leftStrip(OverlayStrip::Side::Left, &engine, &theme);
    OverlayStrip rightStrip(OverlayStrip::Side::Right, &engine, &theme);

    QQmlApplicationEngine qml;
    qml.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    qml.rootContext()->setContextProperty(QStringLiteral("motion"), &engine);
    qml.rootContext()->setContextProperty(QStringLiteral("server"), &server);

    qml.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (qml.rootObjects().isEmpty()) {
        return 1;
    }

    leftStrip.syncVisibility();
    rightStrip.syncVisibility();
    return app.exec();
}
