#include "motion_engine.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kFrameIntervalMs = 25;         // 40 Hz coarse timer for smooth motion + CPU C-states
constexpr qint64 kPhoneDisconnectMs = 3500;  // Tolerates Android's 1 Hz stationary power-save heartbeat
constexpr double kMaxAccelClamp = 50.0;
constexpr double kMaxGyroClamp = 20.0;
constexpr double kGravityTauSec = 0.25;
}

MotionEngine::MotionEngine(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kFrameIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &MotionEngine::step);

    // Low-frequency disconnect & Hz monitor runs independently of overlayEnabled
    // and stops itself automatically when no phone is connected.
    m_watchdogTimer.setInterval(1000);
    m_watchdogTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &MotionEngine::checkImuWatchdog);

    m_clock.start();
    m_hzClock.start();
    m_telemetryClock.start();
    m_lastStepNs = m_clock.nsecsElapsed();
    updateTimerState();
}

bool MotionEngine::isContinuousSim() const
{
    return m_mode == QLatin1String("sim") && m_simPreset != QLatin1String("manual");
}

void MotionEngine::setOverlayEnabled(bool v)
{
    if (m_overlayEnabled == v) return;
    m_overlayEnabled = v;
    emit overlayEnabledChanged();
    updateTimerState();
}

void MotionEngine::setMode(const QString &m)
{
    if (m_mode == m) return;
    m_mode = m;
    m_stillSeconds = 0.0;
    if (m_mode == QLatin1String("sim")) {
        m_imuSource = QStringLiteral("模擬訊號 (Simulation)");
    } else {
        m_imuSource = m_phoneConnected ? m_imuSource : QStringLiteral("等待手機訊號...");
    }
    emit modeChanged();
    emit imuStatusChanged();
    wakeAnimationTimer();
}

void MotionEngine::setSimPreset(const QString &p)
{
    if (m_simPreset == p) return;
    m_simPreset = p;
    m_stillSeconds = 0.0;
    emit simPresetChanged();
    wakeAnimationTimer();
}

void MotionEngine::setDotPairs(int v)
{
    v = std::clamp(v, 3, 6);
    if (m_dotPairs == v) return;
    m_dotPairs = v;
    emit visualsChanged();
}

void MotionEngine::setDotSize(double v)
{
    v = std::clamp(v, 6.0, 24.0);
    if (qFuzzyCompare(m_dotSize, v)) return;
    m_dotSize = v;
    emit visualsChanged();
}

void MotionEngine::setOpacity(double v)
{
    v = std::clamp(v, 0.15, 1.0);
    if (qFuzzyCompare(m_opacity, v)) return;
    m_opacity = v;
    emit visualsChanged();
}

void MotionEngine::setSensitivity(double v)
{
    v = std::clamp(v, 0.3, 2.5);
    if (qFuzzyCompare(m_sensitivity, v)) return;
    m_sensitivity = v;
    emit visualsChanged();
}

void MotionEngine::setEdgeMargin(double v)
{
    v = std::clamp(v, 10.0, 72.0);
    if (qFuzzyCompare(m_edgeMargin, v)) return;
    m_edgeMargin = v;
    emit visualsChanged();
}

void MotionEngine::setAutoHide(bool v)
{
    if (m_autoHide == v) return;
    m_autoHide = v;
    if (!m_autoHide) {
        m_stillSeconds = 0.0;
    }
    emit visualsChanged();
    wakeAnimationTimer();
}

void MotionEngine::setUseAccentColor(bool v)
{
    if (m_useAccentColor == v) return;
    m_useAccentColor = v;
    emit visualsChanged();
}

void MotionEngine::triggerImpulse(double gx, double gy)
{
    if (!std::isfinite(gx) || !std::isfinite(gy)) return;
    m_impX = std::clamp(m_impX + gx, -6.0, 6.0);
    m_impY = std::clamp(m_impY + gy, -6.0, 6.0);
    m_stillSeconds = 0.0;
    wakeAnimationTimer();
}

void MotionEngine::calibrate()
{
    if (m_lastGravityIncluded) {
        // Gravity estimator already removes static DC component; re-seed it cleanly.
        m_hasGravity = false;
        m_biasX = 0.0;
        m_biasY = 0.0;
    } else {
        m_biasX = std::clamp(m_biasX + m_rawLinX, -15.0, 15.0);
        m_biasY = std::clamp(m_biasY + m_rawLinY, -15.0, 15.0);
    }
    // Zero instantaneous residuals so repeated calibrate() clicks are idempotent.
    m_rawLinX = 0.0;
    m_rawLinY = 0.0;
    m_rawGyroZ = 0.0;
    m_filtAx = 0.0;
    m_filtAy = 0.0;
    m_offsetX = 0.0;
    m_offsetY = 0.0;
    m_velX = 0.0;
    m_velY = 0.0;
    m_stillSeconds = 0.0;
    m_calibrationHint = QStringLiteral("已校正原點");
    emit imuStatusChanged();
    emit frameUpdated();
    emit telemetryUpdated();
    wakeAnimationTimer();
}

void MotionEngine::resetDefaults()
{
    setDotPairs(4);
    setDotSize(12.0);
    setOpacity(0.80);
    setSensitivity(1.0);
    setEdgeMargin(26.0);
    setAutoHide(true);
    setUseAccentColor(false);
}

void MotionEngine::ingestImu(double ax, double ay, double az,
                             double gx, double gy, double gz,
                             bool gravityIncluded, const QString &source)
{
    // Guard against NaN / Infinity poisoning from malformed UDP/WS packets.
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) {
        return;
    }

    ax = std::clamp(ax, -kMaxAccelClamp, kMaxAccelClamp);
    ay = std::clamp(ay, -kMaxAccelClamp, kMaxAccelClamp);
    az = std::clamp(az, -kMaxAccelClamp, kMaxAccelClamp);
    gz = std::clamp(gz, -kMaxGyroClamp, kMaxGyroClamp);
    Q_UNUSED(gx);
    Q_UNUSED(gy);

    const qint64 nowMs = m_clock.elapsed();
    const double dtImu = (m_lastImuMs > 0)
        ? std::clamp(double(nowMs - m_lastImuMs) / 1000.0, 0.005, 0.5)
        : 0.04;

    m_lastGravityIncluded = gravityIncluded;
    double linX = ax;
    double linY = ay;

    if (gravityIncluded) {
        if (!m_hasGravity) {
            m_gravX = ax;
            m_gravY = ay;
            m_gravZ = az;
            m_hasGravity = true;
        } else {
            // Packet-rate-independent exponential low-pass tracks static gravity cleanly
            // across both 25 Hz active streaming and 1 Hz stationary heartbeats.
            const double alpha = std::exp(-dtImu / kGravityTauSec);
            m_gravX = alpha * m_gravX + (1.0 - alpha) * ax;
            m_gravY = alpha * m_gravY + (1.0 - alpha) * ay;
            m_gravZ = alpha * m_gravZ + (1.0 - alpha) * az;
        }
        linX = ax - m_gravX;
        linY = ay - m_gravY;
        m_rawLinX = linX;
        m_rawLinY = linY;
    } else {
        m_rawLinX = linX - m_biasX;
        m_rawLinY = linY - m_biasY;
    }
    m_rawGyroZ = gz;

    m_lastImuMs = nowMs;
    m_packetCount++;

    if (!m_watchdogTimer.isActive()) {
        m_hzClock.restart();
        m_watchdogTimer.start();
    }

    if (!m_phoneConnected || m_imuSource != source) {
        const bool newlyConnected = !m_phoneConnected;
        m_phoneConnected = true;
        m_imuSource = source;
        if (newlyConnected && m_mode != QLatin1String("phone")) {
            m_mode = QStringLiteral("phone");
            emit modeChanged();
        }
        emit imuStatusChanged();
    }

    // Only wake the desktop animation timer if the packet actually carries motion above deadzone,
    // or if the kinematics haven't settled yet. Stationary 1 Hz keepalives keep CPU at 0% wakeups!
    const double cueX = std::abs(m_rawLinX + 0.85 * m_rawGyroZ);
    const double cueY = std::abs(m_rawLinY);
    if (cueX > 0.14 || cueY > 0.14 || std::abs(m_offsetX) > 0.05 || std::abs(m_offsetY) > 0.05) {
        wakeAnimationTimer();
    }
}

void MotionEngine::checkImuWatchdog()
{
    const double elapsedSec = std::max(m_hzClock.elapsed() / 1000.0, 0.001);
    const int hz = qRound(m_packetCount / elapsedSec);
    m_packetCount = 0;
    m_hzClock.restart();

    const bool stillAlive = (m_lastImuMs > 0) && ((m_clock.elapsed() - m_lastImuMs) < kPhoneDisconnectMs);
    bool changed = (hz != m_imuHz) || (stillAlive != m_phoneConnected);
    m_imuHz = hz;
    m_phoneConnected = stillAlive;

    if (!stillAlive) {
        m_rawLinX = 0.0;
        m_rawLinY = 0.0;
        m_rawGyroZ = 0.0;
        if (m_mode == QLatin1String("phone")) {
            m_imuSource = QStringLiteral("等待手機訊號...");
            changed = true;
        }
        // Wake animation once to let dots settle back to center / fade out cleanly.
        wakeAnimationTimer();
        m_watchdogTimer.stop();
    }

    if (changed) {
        emit imuStatusChanged();
    }
}

void MotionEngine::wakeAnimationTimer()
{
    if (!m_overlayEnabled) return;
    if (!m_timer.isActive()) {
        m_lastStepNs = m_clock.nsecsElapsed();
        m_timer.start();
    }
}

void MotionEngine::updateTimerState()
{
    if (m_overlayEnabled) {
        wakeAnimationTimer();
    } else {
        m_timer.stop();
        emit frameUpdated();
    }
}

void MotionEngine::step()
{
    const qint64 nowNs = m_clock.nsecsElapsed();
    double dt = double(nowNs - m_lastStepNs) / 1e9;
    m_lastStepNs = nowNs;
    dt = std::clamp(dt, 0.002, 0.06);

    double targetAx = 0.0;
    double targetAy = 0.0;

    if (m_mode == QLatin1String("sim")) {
        m_simTime += dt;
        const double t = m_simTime;
        if (m_simPreset == QLatin1String("city")) {
            const double cycle = std::fmod(t, 14.0);
            if (cycle < 3.2) {
                targetAx = 2.2 * std::sin((cycle / 3.2) * M_PI);
                targetAy = 0.4 * std::sin(t * 2.1);
            } else if (cycle < 6.0) {
                targetAx = 0.15 * std::sin(t * 3.0);
                targetAy = -2.4 * std::sin(((cycle - 3.2) / 2.8) * M_PI);
            } else if (cycle < 9.4) {
                targetAx = -2.3 * std::sin(((cycle - 6.0) / 3.4) * M_PI);
                targetAy = 0.6 * std::sin(t * 1.7);
            } else if (cycle < 12.2) {
                targetAx = 0.2 * std::sin(t * 2.5);
                targetAy = 2.1 * std::sin(((cycle - 9.4) / 2.8) * M_PI);
            }
        } else if (m_simPreset == QLatin1String("highway")) {
            targetAx = 0.85 * std::sin(t * 0.65) + 0.35 * std::sin(t * 1.45);
            targetAy = 0.55 * std::sin(t * 0.95) + 0.25 * std::cos(t * 2.3);
        } else if (m_simPreset == QLatin1String("mountain")) {
            targetAx = 3.1 * std::sin(t * 1.15) + 0.6 * std::sin(t * 2.6);
            targetAy = 1.2 * std::cos(t * 1.15) + 0.4 * std::sin(t * 3.1);
        }
    } else if (m_phoneConnected) {
        targetAx = m_rawLinX + 0.85 * m_rawGyroZ;
        targetAy = m_rawLinY;
    }

    // Add manual test impulses with smooth exponential decay.
    targetAx += m_impX;
    targetAy += m_impY;
    const double impDecay = std::exp(-dt / 0.45);
    m_impX *= impDecay;
    m_impY *= impDecay;
    if (std::abs(m_impX) < 0.01) m_impX = 0.0;
    if (std::abs(m_impY) < 0.01) m_impY = 0.0;

    auto applyDeadzone = [](double v, double dz) {
        if (std::abs(v) < dz) return 0.0;
        return v > 0 ? (v - dz) : (v + dz);
    };
    const double dz = (m_mode == QLatin1String("phone")) ? 0.14 : 0.04;
    targetAx = applyDeadzone(targetAx, dz);
    targetAy = applyDeadzone(targetAy, dz);

    // ~3 Hz low-pass filter to match human vestibular low-frequency sensitivity.
    const double lpAlpha = 1.0 - std::exp(-dt * 18.0);
    m_filtAx += lpAlpha * (targetAx - m_filtAx);
    m_filtAy += lpAlpha * (targetAy - m_filtAy);
    if (targetAx == 0.0 && std::abs(m_filtAx) < 0.005) m_filtAx = 0.0;
    if (targetAy == 0.0 && std::abs(m_filtAy) < 0.005) m_filtAy = 0.0;

    // Inertial coupling: dots move opposite to vehicle acceleration.
    const double maxLateralPx = 30.0 * m_sensitivity;
    const double desiredX = std::clamp(-m_filtAx * 11.0 * m_sensitivity, -maxLateralPx, maxLateralPx);
    const double desiredY = std::clamp(-m_filtAy * 10.0 * m_sensitivity, -26.0 * m_sensitivity, 26.0 * m_sensitivity);

    constexpr double stiffness = 36.0;
    constexpr double damping = 11.5;
    const double accX = stiffness * (desiredX - m_offsetX) - damping * m_velX;
    const double accY = stiffness * (desiredY - m_offsetY) - damping * m_velY;
    m_velX += accX * dt;
    m_velY += accY * dt;
    m_offsetX += m_velX * dt;
    m_offsetY += m_velY * dt;

    // Continuous vertical lane drift (wrapped modulo spanH inside OverlayStrip, no jump!).
    m_scrollY += (-m_filtAy * 38.0 * m_sensitivity) * dt;

    // Auto-hide fade when stationary.
    const double mag = std::hypot(m_filtAx, m_filtAy);
    double targetAlpha = 1.0;
    if (m_autoHide) {
        if (mag < 0.12 && targetAx == 0.0 && targetAy == 0.0) {
            m_stillSeconds += dt;
        } else {
            m_stillSeconds = 0.0;
        }
        targetAlpha = (m_stillSeconds > 3.0) ? 0.0 : 1.0;
        const double fadeRate = (targetAlpha > m_currentAlpha) ? 4.5 : 1.2;
        m_currentAlpha += (targetAlpha - m_currentAlpha) * std::min(1.0, dt * fadeRate);
        if (std::abs(m_currentAlpha - targetAlpha) < 0.005) {
            m_currentAlpha = targetAlpha;
        }
    } else {
        m_currentAlpha += (1.0 - m_currentAlpha) * std::min(1.0, dt * 6.0);
        if (std::abs(1.0 - m_currentAlpha) < 0.005) {
            m_currentAlpha = 1.0;
        }
    }

    // Idle-sleep check: when kinematics & alpha have completely settled, stop the timer (0 Hz CPU wakeups!)
    const bool settled = !isContinuousSim()
        && targetAx == 0.0 && targetAy == 0.0
        && m_impX == 0.0 && m_impY == 0.0
        && m_filtAx == 0.0 && m_filtAy == 0.0
        && std::abs(m_offsetX) < 0.05 && std::abs(m_offsetY) < 0.05
        && std::abs(m_velX) < 0.05 && std::abs(m_velY) < 0.05
        && (!m_autoHide || m_stillSeconds > 3.0)
        && std::abs(m_currentAlpha - targetAlpha) < 0.005;

    if (settled) {
        m_offsetX = 0.0;
        m_offsetY = 0.0;
        m_velX = 0.0;
        m_velY = 0.0;
        m_currentAlpha = targetAlpha;
    }

    // Only emit frameUpdated() if visual output changed beyond perceptual epsilon.
    const bool visualChanged =
        std::abs(m_offsetX - m_lastEmitX) > 0.04 ||
        std::abs(m_offsetY - m_lastEmitY) > 0.04 ||
        std::abs(m_scrollY - m_lastEmitScroll) > 0.08 ||
        std::abs(m_currentAlpha - m_lastEmitAlpha) > 0.004;

    if (visualChanged) {
        m_lastEmitX = m_offsetX;
        m_lastEmitY = m_offsetY;
        m_lastEmitScroll = m_scrollY;
        m_lastEmitAlpha = m_currentAlpha;
        emit frameUpdated();
    }

    // Throttle QML UI telemetry bindings to ~4 Hz (or immediately on settle).
    if (settled || m_telemetryClock.elapsed() >= 250) {
        m_telemetryClock.restart();
        emit telemetryUpdated();
    }

    if (settled) {
        m_timer.stop();
    }
}
