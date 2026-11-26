#include "overlay_window.h"
#include "motion_engine.h"
#include "theme.h"

#ifdef STEADY_LAYER_SHELL
#include <LayerShellQt/Window>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QGuiApplication>
#include <QScreen>
#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QSurfaceFormat>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kStripWidth = 116;
constexpr double kPi = 3.14159265358979323846;
}

#ifdef Q_OS_MACOS
MacOverlayHost *createMacOverlay(QWindow *strip);
void positionMacOverlay(MacOverlayHost *host, const QRect &geometry);
void showMacOverlay(MacOverlayHost *host, bool visible);
void destroyMacOverlay(MacOverlayHost *host, QWindow *strip);
#endif

OverlayStrip::OverlayStrip(Side side, MotionEngine *engine, Theme *theme)
    : QRasterWindow()
    , m_side(side)
    , m_engine(engine)
    , m_theme(theme)
{
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

#ifdef STEADY_LAYER_SHELL
    m_layerShell = QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
#endif
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::WindowTransparentForInput
        | Qt::WindowDoesNotAcceptFocus;
    if (!m_layerShell) {
        flags |= Qt::WindowStaysOnTopHint | Qt::Tool | Qt::NoDropShadowWindowHint;
    }
    setFlags(flags);

    if (m_layerShell) {
        resize(kStripWidth, 800);
        configureLayerShell();
    } else {
#ifdef Q_OS_MACOS
        m_macHost = createMacOverlay(this);
#endif
        connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, &OverlayStrip::followScreen);
        followScreen(QGuiApplication::primaryScreen());
    }

    connect(m_engine, &MotionEngine::frameUpdated, this, &OverlayStrip::scheduleRepaintIfNeeded);
    connect(m_engine, &MotionEngine::visualsChanged, this, &OverlayStrip::scheduleRepaintIfNeeded);
    connect(m_engine, &MotionEngine::overlayEnabledChanged, this, &OverlayStrip::syncVisibility);
    connect(m_theme, &Theme::changed, this, &OverlayStrip::scheduleRepaintIfNeeded);
}

OverlayStrip::~OverlayStrip()
{
#ifdef Q_OS_MACOS
    if (m_macHost) destroyMacOverlay(m_macHost, this);
#endif
}

void OverlayStrip::configureLayerShell()
{
#ifdef STEADY_LAYER_SHELL
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
#endif
}

void OverlayStrip::followScreen(QScreen *screen)
{
    disconnect(m_geometryConnection);
    if (!screen) return;

#ifdef Q_OS_MACOS
    if (!m_macHost)
#endif
        setScreen(screen);
    const auto position = [this, screen]() {
        const QRect area = screen->geometry();
        const int x = m_side == Side::Left ? area.left() : area.right() - kStripWidth + 1;
        m_edgeGeometry = QRect(x, area.top(), kStripWidth, area.height());
#ifdef Q_OS_MACOS
        if (m_macHost) {
            positionMacOverlay(m_macHost, m_edgeGeometry);
            resize(m_edgeGeometry.size());
            return;
        }
#endif
        setGeometry(m_edgeGeometry);
    };
    m_geometryConnection = connect(screen, &QScreen::geometryChanged, this, position);
    position();
    if (isVisible()) applyNativeBehavior();
}

void OverlayStrip::applyNativeBehavior()
{
#ifdef Q_OS_WIN
    const auto hwnd = reinterpret_cast<HWND>(winId());
    constexpr LONG_PTR required = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOPMOST
        | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    const LONG_PTR previous = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((previous & required) != required || (previous & WS_EX_APPWINDOW)) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (previous | required) & ~LONG_PTR(WS_EX_APPWINDOW));
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS")) {
        const auto flags = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        qWarning("overlay %s exstyle=0x%lx transparent=%d layered=%d topmost=%d toolwindow=%d noactivate=%d appwindow=%d",
                 m_side == Side::Left ? "left" : "right", static_cast<unsigned long>(flags),
                 bool(flags & WS_EX_TRANSPARENT), bool(flags & WS_EX_LAYERED),
                 bool(flags & WS_EX_TOPMOST), bool(flags & WS_EX_TOOLWINDOW),
                 bool(flags & WS_EX_NOACTIVATE), bool(flags & WS_EX_APPWINDOW));
    }
#endif
}

void OverlayStrip::scheduleRepaintIfNeeded()
{
    if (!isVisible()) return;
    QRegion dirty = m_paintedDots;
    for (const Dot &dot : dots()) {
        dirty += dotBounds(dot);
    }
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

void OverlayStrip::syncVisibility()
{
    if (m_engine->overlayEnabled()) {
        if (!isVisible()) {
            show();
#ifdef Q_OS_MACOS
            if (m_macHost) showMacOverlay(m_macHost, true);
#endif
            if (!m_layerShell) applyNativeBehavior();
            if (qEnvironmentVariableIsSet("STEADY_OVERLAY_DIAGNOSTICS")) {
                qWarning("overlay %s platform=%s geometry=%d,%d %dx%d",
                         m_side == Side::Left ? "left" : "right", qPrintable(QGuiApplication::platformName()),
                         m_layerShell ? x() : m_edgeGeometry.x(), m_layerShell ? y() : m_edgeGeometry.y(),
                         width(), height());
            }
        }
        update();
    } else {
        hide();
#ifdef Q_OS_MACOS
        if (m_macHost) showMacOverlay(m_macHost, false);
#endif
        m_paintedDots = {};
    }
}

QRect OverlayStrip::dotBounds(const Dot &dot) const
{
    // Include the halo and antialiased pixels on each edge.
    const double extent = m_engine->dotSize() * 0.5 + 3.2;
    const QPointF corner(extent, extent);
    return QRectF(dot.center - corner, dot.center + corner).toAlignedRect();
}

std::vector<OverlayStrip::Dot> OverlayStrip::dots() const
{
    const double alpha = m_engine->opacity() * m_engine->currentAlpha();
    if (!m_engine->overlayEnabled() || alpha <= MotionEngine::minimumVisibleOpacity || width() < 64 || height() < 2) {
        return {};
    }

    const int pairs = std::max(1, m_engine->dotPairs());
    const double radius = m_engine->dotSize() * 0.5;
    const double inset = m_engine->edgeMargin() + 30.0;
    const double centerX = (m_side == Side::Left)
        ? std::clamp(inset, 26.0, width() - 26.0)
        : std::clamp(width() - inset, 26.0, width() - 26.0);

    const double topPad = height() * 0.12;
    const double spanH = height() * 0.76;
    const double spacing = spanH / pairs;

    const double dx = m_engine->offsetX();
    const double dy = m_engine->offsetY();
    const double scroll = m_engine->scrollY();

    std::vector<Dot> result;
    result.reserve(pairs);

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
        const double parallaxX = dx * (1.0 + 0.12 * std::cos(normY * kPi));
        const double x = std::clamp(centerX + parallaxX, minX, maxX);

        result.push_back({QPointF(x, y), a});
    }
    return result;
}

void OverlayStrip::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    const QRegion dirty = event->region();
    p.setClipRegion(dirty);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &rect : dirty) {
        p.fillRect(rect, Qt::transparent);
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double radius = m_engine->dotSize() * 0.5;
    const QColor coreColor = m_engine->useAccentColor() ? m_theme->accent() : QColor(246, 248, 250);
    const QColor rimColor(18, 18, 22);

    m_paintedDots = m_paintedDots.intersected(QRect(QPoint(), size())) - dirty;
    for (const Dot &dot : dots()) {
        const QRegion painted = dirty.intersected(dotBounds(dot));
        if (painted.isEmpty()) continue;
        m_paintedDots += painted;

        QColor halo = rimColor;
        halo.setAlphaF(dot.alpha * 0.42);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(dot.center, radius + 2.2, radius + 2.2);

        QColor fill = coreColor;
        fill.setAlphaF(dot.alpha * 0.92);
        QColor border = rimColor;
        border.setAlphaF(dot.alpha * 0.65);
        p.setPen(QPen(border, 1.25));
        p.setBrush(fill);
        p.drawEllipse(dot.center, radius, radius);
    }
}
