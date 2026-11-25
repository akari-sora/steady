#pragma once

#include <QRasterWindow>

class MotionEngine;
class Theme;

// Narrow edge-strip overlay window (left or right). Using two slim strips
// instead of a full-screen SHM buffer cuts CPU/memory bandwidth by ~80%
// and leaves the center of the display completely untouched.
class OverlayStrip : public QRasterWindow {
    Q_OBJECT

public:
    enum class Side { Left, Right };

    OverlayStrip(Side side, MotionEngine *engine, Theme *theme);

    void syncVisibility();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void configureLayerShell();
    void scheduleRepaintIfNeeded();

    Side m_side;
    MotionEngine *m_engine;
    Theme *m_theme;
    bool m_configured = false;
    bool m_lastPaintedTransparent = false;
};
