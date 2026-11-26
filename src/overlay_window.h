#pragma once

#include <QRasterWindow>
#include <QRegion>
#include <vector>

class MotionEngine;
class Theme;
class QScreen;
#ifdef Q_OS_MACOS
struct MacOverlayHost;
#endif

// Narrow edge-strip overlay window (left or right). Using two slim strips
// instead of a full-screen SHM buffer cuts CPU/memory bandwidth by ~80%
// and leaves the center of the display completely untouched.
class OverlayStrip : public QRasterWindow {
    Q_OBJECT

public:
    enum class Side { Left, Right };

    OverlayStrip(Side side, MotionEngine *engine, Theme *theme);
    ~OverlayStrip() override;

    void syncVisibility();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Dot {
        QPointF center;
        double alpha;
    };

    std::vector<Dot> dots() const;
    QRect dotBounds(const Dot &dot) const;
    void configureLayerShell();
    void followScreen(QScreen *screen);
    void applyNativeBehavior();
    QRect m_edgeGeometry;
    void scheduleRepaintIfNeeded();

    Side m_side;
    MotionEngine *m_engine;
    Theme *m_theme;
    bool m_layerShell = false;
    QMetaObject::Connection m_geometryConnection;
    QRegion m_paintedDots;
#ifdef Q_OS_MACOS
    MacOverlayHost *m_macHost = nullptr;
#endif
};
