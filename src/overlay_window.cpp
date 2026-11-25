#include "overlay_window.h"
#include "motion_engine.h"
#include "theme.h"

#include <LayerShellQt/Window>
#include <QColor>
#include <QPainter>
#include <QPen>
#include <QSurfaceFormat>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kStripWidth = 116;
}

OverlayStrip::OverlayStrip(Side side, MotionEngine *engine, Theme *theme)
    : QRasterWindow()
    , m_side(side)
    , m_engine(engine)
    , m_theme(theme)
{
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    resize(kStripWidth, 800);

    configureLayerShell();

    connect(m_engine, &MotionEngine::frameUpdated, this, &OverlayStrip::scheduleRepaintIfNeeded);
    connect(m_engine, &MotionEngine::visualsChanged, this, [this]() {
        m_lastPaintedTransparent = false;
        scheduleRepaintIfNeeded();
    });
    connect(m_engine, &MotionEngine::overlayEnabledChanged, this, &OverlayStrip::syncVisibility);
    connect(m_theme, &Theme::changed, this, [this]() {
        m_lastPaintedTransparent = false;
        scheduleRepaintIfNeeded();
    });
}

void OverlayStrip::configureLayerShell()
{
    if (m_configured) return;
    auto *ls = LayerShellQt::Window::get(this);
    ls->setLayer(LayerShellQt::Window::LayerOverlay);

    LayerShellQt::Window::Anchors anchors = LayerShellQt::Window::AnchorTop;
    anchors |= LayerShellQt::Window::AnchorBottom;
    anchors |= (m_side == Side::Left)
        ? LayerShellQt::Window::AnchorLeft
        : LayerShellQt::Window::AnchorRight;
    ls->setAnchors(anchors);

    ls->setDesiredSize(QSize(kStripWidth, 0));
    ls->setExclusiveZone(-1);
    ls->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    ls->setScope(m_side == Side::Left
        ? QStringLiteral("steady-overlay-left")
        : QStringLiteral("steady-overlay-right"));
    ls->setActivateOnShow(false);
    m_configured = true;
}

void OverlayStrip::scheduleRepaintIfNeeded()
{
    if (!isVisible()) return;
    const double alpha = m_engine->opacity() * m_engine->currentAlpha();
    const bool nowTransparent = !m_engine->overlayEnabled() || alpha <= 0.01;
    // Avoid redundant Wayland SHM commits once an empty transparent frame has been presented.
    if (nowTransparent && m_lastPaintedTransparent) {
        return;
    }
    requestUpdate();
}

void OverlayStrip::syncVisibility()
{
    if (m_engine->overlayEnabled()) {
        m_lastPaintedTransparent = false;
        if (!isVisible()) {
            show();
        }
        requestUpdate();
    } else {
        if (isVisible()) {
            hide();
        }
    }
}

void OverlayStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (width() < 64 || height() <= 0) {
        return;
    }

    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(0, 0, width(), height(), Qt::transparent);

    const double alpha = m_engine->opacity() * m_engine->currentAlpha();
    if (!m_engine->overlayEnabled() || alpha <= 0.01) {
        m_lastPaintedTransparent = true;
        return;
    }
    m_lastPaintedTransparent = false;

    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int pairs = std::max(1, m_engine->dotPairs());
    const double radius = m_engine->dotSize() * 0.5;
    const double inset = m_engine->edgeMargin() + 30.0;
    const double centerX = (m_side == Side::Left)
        ? std::clamp(inset, 26.0, width() - 26.0)
        : std::clamp(width() - inset, 26.0, width() - 26.0);

    const double topPad = height() * 0.12;
    const double spanH = height() * 0.76;
    if (spanH <= 1.0) return;
    const double spacing = spanH / pairs;

    const double dx = m_engine->offsetX();
    const double dy = m_engine->offsetY();
    const double scroll = m_engine->scrollY();

    const QColor coreColor = m_engine->useAccentColor() ? m_theme->accent() : QColor(246, 248, 250);
    const QColor rimColor(18, 18, 22);

    const double minX = radius + 3.0;
    const double maxX = std::max(minX, width() - radius - 3.0);

    for (int i = 0; i < pairs; ++i) {
        double rawPos = std::fmod((i + 0.5) * spacing + scroll + dy, spanH);
        if (rawPos < 0.0) rawPos += spanH;
        const double y = topPad + rawPos;

        const double distFromEdge = std::min(rawPos, spanH - rawPos);
        const double edgeFade = std::clamp(distFromEdge / (spacing * 0.35), 0.0, 1.0);
        const double a = std::clamp(alpha * edgeFade, 0.0, 1.0);
        if (a <= 0.01) continue;

        const double normY = (rawPos / spanH) - 0.5;
        const double parallaxX = dx * (1.0 + 0.12 * std::cos(normY * M_PI));
        const double x = std::clamp(centerX + parallaxX, minX, maxX);

        QColor halo = rimColor;
        halo.setAlphaF(a * 0.42);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(QPointF(x, y), radius + 2.2, radius + 2.2);

        QColor fill = coreColor;
        fill.setAlphaF(a * 0.92);
        QColor border = rimColor;
        border.setAlphaF(a * 0.65);
        p.setPen(QPen(border, 1.25));
        p.setBrush(fill);
        p.drawEllipse(QPointF(x, y), radius, radius);
    }
}
