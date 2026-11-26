#include "theme.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

Theme::Theme(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_LINUX
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (!stateDir.isEmpty()) {
        m_colorsPath = QDir(stateDir).filePath(QStringLiteral("omarchy/current/theme/colors.toml"));
    }
#endif
    qGuiApp->installEventFilter(this);
    reload();
    armWatcher();

    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        QTimer::singleShot(100, this, [this]() {
            reload();
            armWatcher();
        });
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        QTimer::singleShot(100, this, [this]() {
            reload();
            armWatcher();
        });
    });
}

bool Theme::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) reload();
    return QObject::eventFilter(watched, event);
}

void Theme::armWatcher()
{
    if (!m_watcher.files().isEmpty()) {
        m_watcher.removePaths(m_watcher.files());
    }
    if (!m_watcher.directories().isEmpty()) {
        m_watcher.removePaths(m_watcher.directories());
    }
    if (m_colorsPath.isEmpty()) return;
    QFileInfo fi(m_colorsPath);
    if (fi.exists()) {
        m_watcher.addPath(m_colorsPath);
    }
    QString dir = fi.absolutePath();
    if (QDir(dir).exists()) {
        m_watcher.addPath(dir);
    }
}

void Theme::reload()
{
    QFile f(m_colorsPath);
    const bool omarchy = !m_colorsPath.isEmpty() && f.open(QIODevice::ReadOnly | QIODevice::Text);

    QColor newAccent("#FFD60A");
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (!omarchy) {
        const QColor accent = QGuiApplication::palette().color(QPalette::Accent);
        if (accent.isValid()) newAccent = accent;
    }
#endif
    QColor newBg("#0c0b0c");
    QColor newSurface("#171517");
    QColor newFg("#FAFCFB");
    QColor newMuted("#8a8184");

    static const QRegularExpression rx(R"RE(^\s*([a-zA-Z0-9_]+)\s*=\s*"(#[0-9a-fA-F]{6,8})")RE");
    QTextStream ts(&f);
    while (omarchy && !ts.atEnd()) {
        const QString line = ts.readLine();
        const auto m = rx.match(line);
        if (!m.hasMatch()) continue;
        const QString key = m.captured(1);
        const QColor col(m.captured(2));
        if (!col.isValid()) continue;

        if (key == "accent") newAccent = col;
        else if (key == "background") newBg = col;
        else if (key == "lighter_background") newSurface = col.lighter(135);
        else if (key == "foreground") newFg = col;
        else if (key == "muted") newMuted = col.lighter(135);
    }
    if (newSurface == newBg) {
        newSurface = newBg.lighter(150);
    }

    if (omarchy == m_omarchy && newAccent == m_accent && newBg == m_background &&
        newSurface == m_surface && newFg == m_foreground && newMuted == m_muted) {
        return;
    }
    m_omarchy = omarchy;
    m_accent = newAccent;
    m_background = newBg;
    m_surface = newSurface;
    m_foreground = newFg;
    m_muted = newMuted;
    emit changed();
}
