#include "theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

Theme::Theme(QObject *parent)
    : QObject(parent)
    , m_colorsPath(QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml")
{
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

void Theme::armWatcher()
{
    if (!m_watcher.files().isEmpty()) {
        m_watcher.removePaths(m_watcher.files());
    }
    if (!m_watcher.directories().isEmpty()) {
        m_watcher.removePaths(m_watcher.directories());
    }
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
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QColor newAccent = m_accent;
    QColor newBg = m_background;
    QColor newSurface = m_surface;
    QColor newFg = m_foreground;
    QColor newMuted = m_muted;

    static const QRegularExpression rx(R"RE(^\s*([a-zA-Z0-9_]+)\s*=\s*"(#[0-9a-fA-F]{6,8})")RE");
    QTextStream ts(&f);
    while (!ts.atEnd()) {
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

    if (newAccent == m_accent && newBg == m_background &&
        newSurface == m_surface && newFg == m_foreground && newMuted == m_muted) {
        return;
    }
    m_accent = newAccent;
    m_background = newBg;
    m_surface = newSurface;
    m_foreground = newFg;
    m_muted = newMuted;
    emit changed();
}
