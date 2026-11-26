#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor surface READ surface NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(bool omarchy READ omarchy NOTIFY changed)
    Q_PROPERTY(QColor muted READ muted NOTIFY changed)

public:
    explicit Theme(QObject *parent = nullptr);

    QColor accent() const { return m_accent; }
    QColor background() const { return m_background; }
    QColor surface() const { return m_surface; }
    QColor foreground() const { return m_foreground; }
    bool omarchy() const { return m_omarchy; }
    QColor muted() const { return m_muted; }

signals:
    void changed();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void reload();
    void armWatcher();

    QString m_colorsPath;
    bool m_omarchy = false;
    QFileSystemWatcher m_watcher;
    QColor m_accent{"#FFD60A"};
    QColor m_background{"#0c0b0c"};
    QColor m_surface{"#171517"};
    QColor m_foreground{"#FAFCFB"};
    QColor m_muted{"#8a8184"};
};
