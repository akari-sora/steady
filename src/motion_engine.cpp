#include "motion_engine.h"

#include <QSettings>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kFrameIntervalMs = 25;         // 40 Hz coarse timer for smooth motion + CPU C-states
constexpr qint64 kPhoneDisconnectMs = 3500;  // Tolerates Android's 1 Hz stationary power-save heartbeat
constexpr double kMaxAccelClamp = 50.0;
constexpr double kMaxGyroClamp = 20.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAutoHideDelaySec = 3.0;
constexpr double kGravityTauSec = 20.0;
constexpr int kSimulationTimeoutMs = 10 * 60 * 1000;
}

MotionEngine::MotionEngine(Timing timing, QObject *parent)
    : MotionEngine(timing, SimulationTimeout::Enabled, parent)
{
}

MotionEngine::MotionEngine(Timing timing, SimulationTimeout timeout, QObject *parent)
    : QObject(parent)
    , m_timing(timing)
    , m_simulationTimeout(timeout)
{
    m_timer.setInterval(kFrameIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &MotionEngine::step);

    m_fadeDelay.setSingleShot(true);
    m_fadeDelay.setTimerType(Qt::PreciseTimer);
    connect(&m_fadeDelay, &QTimer::timeout, this, &MotionEngine::beginFade);

    // Low-frequency disconnect & Hz monitor runs independently of overlayEnabled
    // and stops itself automatically when no phone is connected.
    m_watchdogTimer.setInterval(1000);
    m_watchdogTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &MotionEngine::checkImuWatchdog);

    m_simTimeout.setSingleShot(true);
    m_simTimeout.setTimerType(Qt::PreciseTimer);
    connect(&m_simTimeout, &QTimer::timeout, this, &MotionEngine::pauseSimulation);

    m_clock.start();
    m_stillSeconds = kAutoHideDelaySec;
}

bool MotionEngine::isContinuousSim() const
{
    return m_mode == QLatin1String("sim") && m_simPreset != QLatin1String("manual") && !m_simulationPaused;
}

void MotionEngine::updateSimulationTimeout()
{
    if (isContinuousSim()) {
        noteInteraction();
    } else {
        m_simDeadlineMs = -1;
        m_simTimeout.stop();
    }
}

void MotionEngine::noteInteraction()
{
    if (m_simulationTimeout == SimulationTimeout::Disabled || !isContinuousSim()) return;
    m_simDeadlineMs = nowMs() + kSimulationTimeoutMs;
    if (m_timing == Timing::Realtime) m_simTimeout.start(kSimulationTimeoutMs);
}

void MotionEngine::pauseSimulation()
{
    m_simulationPaused = true;
    m_simDeadlineMs = -1;
    emit simulationPausedChanged();
}

void MotionEngine::resumeSimulation()
{
    if (!m_simulationPaused) return;
    m_simulationPaused = false;
    m_stillSeconds = 0.0;
    updateSimulationTimeout();
    emit simulationPausedChanged();
    wakeAnimationTimer();
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
    if (m != QLatin1String("phone") && m != QLatin1String("sim")) return;
    if (m_mode == m) {
        resumeSimulation();
        return;
    }
    m_mode = m;
    m_stillSeconds = 0.0;
    if (m_mode == QLatin1String("sim")) {
        m_imuSource = QStringLiteral("模擬訊號 (Simulation)");
    } else {
        m_imuSource = m_phoneConnected ? m_imuSource : QStringLiteral("等待手機訊號...");
    }
    resumeSimulation();
    updateSimulationTimeout();
    emit modeChanged();
    emit imuStatusChanged();
    wakeAnimationTimer();
}

void MotionEngine::setSimPreset(const QString &p)
{
    if (!validPreset(p)) return;
    if (m_simPreset == p) {
        resumeSimulation();
        return;
    }
    m_simPreset = p;
    resumeSimulation();
    updateSimulationTimeout();
    emit simPresetChanged();
    if (m_mode == QLatin1String("sim")) {
        m_stillSeconds = 0.0;
        wakeAnimationTimer();
    }
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
    if (!std::isfinite(v)) return;
    v = std::clamp(v, 6.0, 24.0);
    if (qFuzzyCompare(m_dotSize, v)) return;
    m_dotSize = v;
    emit visualsChanged();
}

void MotionEngine::setOpacity(double v)
{
    if (!std::isfinite(v)) return;
    v = std::clamp(v, 0.15, 1.0);
    if (qFuzzyCompare(m_opacity, v)) return;
    m_opacity = v;
    emit visualsChanged();
}

void MotionEngine::setSensitivity(double v)
{
    if (!std::isfinite(v)) return;
    v = std::clamp(v, 0.3, 2.5);
    if (qFuzzyCompare(m_sensitivity, v)) return;
    m_sensitivity = v;
    emit visualsChanged();
}

void MotionEngine::setEdgeMargin(double v)
{
    if (!std::isfinite(v)) return;
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
    resumeSimulation();
    noteInteraction();
    m_impX = std::clamp(m_impX + gx, -6.0, 6.0);
    m_impY = std::clamp(m_impY + gy, -6.0, 6.0);
    m_stillSeconds = 0.0;
    wakeAnimationTimer();
}

void MotionEngine::calibrate()
{
    if (m_lastImuMs >= 0 && m_lastGravityIncluded) {
        m_gravX = m_lastImuX;
        m_gravY = m_lastImuY;
        m_gravZ = m_lastImuZ;
        m_hasGravity = true;
        m_biasX = 0.0;
        m_biasY = 0.0;
    } else if (m_lastImuMs >= 0) {
        m_biasX = std::clamp(m_lastImuX, -15.0, 15.0);
        m_biasY = std::clamp(m_lastImuY, -15.0, 15.0);
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

void MotionEngine::loadSettings(const QSettings &settings)
{
    setMode(settings.value(QStringLiteral("mode"), m_mode).toString());
    setSimPreset(settings.value(QStringLiteral("preset"), m_simPreset).toString());
    bool ok = false;
    const int pairs = settings.value(QStringLiteral("dotPairs")).toInt(&ok);
    if (ok) setDotPairs(pairs);
    const auto number = [&settings](const QString &key, double fallback) {
        bool valid = false;
        const double value = settings.value(key).toDouble(&valid);
        return valid && std::isfinite(value) ? value : fallback;
    };
    setDotSize(number(QStringLiteral("dotSize"), m_dotSize));
    setOpacity(number(QStringLiteral("opacity"), m_opacity));
    setSensitivity(number(QStringLiteral("sensitivity"), m_sensitivity));
    setEdgeMargin(number(QStringLiteral("margin"), m_edgeMargin));
    setAutoHide(settings.value(QStringLiteral("autoHide"), m_autoHide).toBool());
    setUseAccentColor(settings.value(QStringLiteral("accent"), m_useAccentColor).toBool());
}

void MotionEngine::saveSettings(QSettings &settings) const
{
    settings.setValue(QStringLiteral("mode"), m_mode);
    settings.setValue(QStringLiteral("preset"), m_simPreset);
    settings.setValue(QStringLiteral("dotPairs"), m_dotPairs);
    settings.setValue(QStringLiteral("dotSize"), m_dotSize);
    settings.setValue(QStringLiteral("opacity"), m_opacity);
    settings.setValue(QStringLiteral("sensitivity"), m_sensitivity);
    settings.setValue(QStringLiteral("margin"), m_edgeMargin);
    settings.setValue(QStringLiteral("autoHide"), m_autoHide);
    settings.setValue(QStringLiteral("accent"), m_useAccentColor);
}

void MotionEngine::disconnectImu()
{
    if (!m_phoneConnected) return;
    m_lastImuMs = -1;
    m_packetCount = 0;
    checkImuWatchdog();
}

void MotionEngine::resetImuCalibration()
{
    m_hasGravity = false;
    m_lastGravityIncluded = false;
    m_biasX = 0.0;
    m_biasY = 0.0;
    m_calibrationHint = QStringLiteral("尚未校正");
}

bool MotionEngine::ingestImu(double ax, double ay, double az,
                             double gx, double gy, double gz,
                             bool gravityIncluded, const QString &source, bool idle)
{
    // Guard against NaN / Infinity poisoning from malformed UDP/WS packets.
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az) ||
        !std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(gz)) {
        return false;
    }

    ax = std::clamp(ax, -kMaxAccelClamp, kMaxAccelClamp);
    ay = std::clamp(ay, -kMaxAccelClamp, kMaxAccelClamp);
    az = std::clamp(az, -kMaxAccelClamp, kMaxAccelClamp);
    gz = std::clamp(gz, -kMaxGyroClamp, kMaxGyroClamp);
    Q_UNUSED(gx);
    Q_UNUSED(gy);

    const qint64 receivedMs = nowMs();
    const double dtImu = (m_lastImuMs >= 0)
        ? std::clamp(double(receivedMs - m_lastImuMs) / 1000.0, 0.005, 0.5)
        : 0.04;
    m_lastImuX = ax;
    m_lastImuY = ay;
    m_lastImuZ = az;
    m_lastGravityIncluded = gravityIncluded;

    if (idle) {
        m_rawLinX = 0.0;
        m_rawLinY = 0.0;
        m_rawGyroZ = 0.0;
    } else if (gravityIncluded) {
        if (!m_hasGravity) {
            m_gravX = ax;
            m_gravY = ay;
            m_gravZ = az;
            m_hasGravity = true;
        } else {
            // Slowly track gravity so sustained turns and braking retain their cue.
            // Calibration re-seeds the estimate when the phone's mounting angle changes.
            const double alpha = std::exp(-dtImu / kGravityTauSec);
            m_gravX = alpha * m_gravX + (1.0 - alpha) * ax;
            m_gravY = alpha * m_gravY + (1.0 - alpha) * ay;
            m_gravZ = alpha * m_gravZ + (1.0 - alpha) * az;
        }
        m_rawLinX = ax - m_gravX;
        m_rawLinY = ay - m_gravY;
    } else {
        m_rawLinX = ax - m_biasX;
        m_rawLinY = ay - m_biasY;
    }
    if (!idle) m_rawGyroZ = gz;

    m_lastImuMs = receivedMs;
    m_packetCount++;

    if (!m_phoneConnected) {
        m_lastHzMs = receivedMs;
        if (m_timing == Timing::Realtime) m_watchdogTimer.start();
    }

    if (!m_phoneConnected || m_imuSource != source) {
        const bool newlyConnected = !m_phoneConnected;
        m_phoneConnected = true;
        m_imuSource = source;
        if (newlyConnected && m_mode != QLatin1String("phone")) {
            m_mode = QStringLiteral("phone");
            if (m_simulationPaused) {
                m_simulationPaused = false;
                emit simulationPausedChanged();
            }
            updateSimulationTimeout();
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
    return true;
}

void MotionEngine::checkImuWatchdog()
{
    const qint64 currentMs = nowMs();
    const double elapsedSec = std::max(double(currentMs - m_lastHzMs) / 1000.0, 0.001);
    const int hz = qRound(m_packetCount / elapsedSec);
    m_packetCount = 0;
    m_lastHzMs = currentMs;

    const bool stillAlive = (m_lastImuMs >= 0) && (currentMs - m_lastImuMs < kPhoneDisconnectMs);
    bool changed = (hz != m_imuHz) || (stillAlive != m_phoneConnected);
    m_imuHz = hz;
    m_phoneConnected = stillAlive;

    if (!stillAlive) {
        m_lastImuMs = -1;
        m_rawLinX = 0.0;
        m_rawLinY = 0.0;
        m_rawGyroZ = 0.0;
        if (m_mode == QLatin1String("phone")) {
            m_imuSource = QStringLiteral("等待手機訊號...");
            changed = true;
        }
        m_watchdogTimer.stop();
    }

    if (changed) {
        emit imuStatusChanged();
    }
}

qint64 MotionEngine::nowNs() const
{
    return m_timing == Timing::Manual ? m_manualNs : m_clock.nsecsElapsed();
}

void MotionEngine::beginFade()
{
    m_stillSeconds = kAutoHideDelaySec;
    wakeAnimationTimer();
}

void MotionEngine::cancelFadeDelay()
{
    m_fadeAtNs = -1;
    m_fadeDelay.stop();
}

void MotionEngine::wakeAnimationTimer()
{
    if (!m_overlayEnabled) return;
    cancelFadeDelay();
    if (!m_animationActive) {
        m_animationActive = true;
        m_lastStepNs = nowNs();
        if (m_timing == Timing::Realtime) m_timer.start();
    }
}

void MotionEngine::updateTimerState()
{
    if (m_overlayEnabled) {
        wakeAnimationTimer();
    } else {
        m_animationActive = false;
        m_timer.stop();
        cancelFadeDelay();
        emit frameUpdated();
    }
}

bool MotionEngine::validPreset(const QString &preset)
{
    return preset == QLatin1String("city") || preset == QLatin1String("highway")
        || preset == QLatin1String("mountain") || preset == QLatin1String("manual");
}

MotionEngine::Acceleration MotionEngine::simulationSample(const QString &preset, double t)
{
    double targetAx = 0.0;
    double targetAy = 0.0;
    if (preset == QLatin1String("city")) {
        const double cycle = std::fmod(t, 14.0);
        if (cycle < 3.2) {
            targetAx = 2.2 * std::sin((cycle / 3.2) * kPi);
            targetAy = 0.4 * std::sin(t * 2.1);
        } else if (cycle < 6.0) {
            targetAx = 0.15 * std::sin(t * 3.0);
            targetAy = -2.4 * std::sin(((cycle - 3.2) / 2.8) * kPi);
        } else if (cycle < 9.4) {
            targetAx = -2.3 * std::sin(((cycle - 6.0) / 3.4) * kPi);
            targetAy = 0.6 * std::sin(t * 1.7);
        } else if (cycle < 12.2) {
            targetAx = 0.2 * std::sin(t * 2.5);
            targetAy = 2.1 * std::sin(((cycle - 9.4) / 2.8) * kPi);
        }
    } else if (preset == QLatin1String("highway")) {
        targetAx = 0.85 * std::sin(t * 0.65) + 0.35 * std::sin(t * 1.45);
        targetAy = 0.55 * std::sin(t * 0.95) + 0.25 * std::cos(t * 2.3);
    } else if (preset == QLatin1String("mountain")) {
        targetAx = 3.1 * std::sin(t * 1.15) + 0.6 * std::sin(t * 2.6);
        targetAy = 1.2 * std::cos(t * 1.15) + 0.4 * std::sin(t * 3.1);
    }
    return {targetAx, targetAy};
}

void MotionEngine::step()
{
    const qint64 currentNs = nowNs();
    const double dt = double(currentNs - m_lastStepNs) / 1e9;
    m_lastStepNs = currentNs;
    advance(std::clamp(dt, 0.002, 0.06));
}

void MotionEngine::advance(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    if (m_timing == Timing::Realtime) {
        if (m_animationActive) animate(std::min(seconds, 0.06));
        return;
    }

    constexpr qint64 frameNs = kFrameIntervalMs * 1'000'000LL;
    while (seconds > 0.0) {
        const qint64 ns = qRound64(std::min(seconds, double(frameNs) / 1e9) * 1e9);
        if (ns == 0) break;
        seconds -= double(ns) / 1e9;
        m_manualNs += ns;
        const qint64 ms = nowMs();
        if (m_simDeadlineMs >= 0 && ms >= m_simDeadlineMs) pauseSimulation();
        if (m_phoneConnected && (ms - m_lastHzMs >= 1000 || ms - m_lastImuMs >= kPhoneDisconnectMs)) {
            checkImuWatchdog();
        }
        if (m_animationActive) {
            animate(double(ns) / 1e9);
        } else if (m_fadeAtNs >= 0 && m_manualNs >= m_fadeAtNs) {
            beginFade();
        }
    }
}

void MotionEngine::animate(double dt)
{
    Acceleration target{};
    if (isContinuousSim()) {
        m_simTime += dt;
        target = simulationSample(m_simPreset, m_simTime);
    } else if (m_mode == QLatin1String("phone") && m_phoneConnected) {
        target = {m_rawLinX + 0.85 * m_rawGyroZ, m_rawLinY};
    }
    double targetAx = target.x;
    double targetAy = target.y;

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
        targetAlpha = (m_stillSeconds >= kAutoHideDelaySec) ? 0.0 : 1.0;
        const double fadeRate = (targetAlpha > m_currentAlpha) ? 4.5 : 1.2;
        m_currentAlpha += (targetAlpha - m_currentAlpha) * std::min(1.0, dt * fadeRate);
        if (std::abs(m_currentAlpha - targetAlpha) < 0.005
            || (targetAlpha == 0.0 && m_currentAlpha * m_opacity <= minimumVisibleOpacity)) {
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
        std::abs(m_currentAlpha - m_lastEmitAlpha) > 0.004 ||
        (settled && m_currentAlpha != m_lastEmitAlpha);

    if (visualChanged) {
        m_lastEmitX = m_offsetX;
        m_lastEmitY = m_offsetY;
        m_lastEmitScroll = m_scrollY;
        m_lastEmitAlpha = m_currentAlpha;
        emit frameUpdated();
    }

    // Throttle QML UI telemetry bindings to ~4 Hz (or immediately on settle).
    const qint64 ms = nowMs();
    if (settled || ms - m_lastTelemetryMs >= 250) {
        m_lastTelemetryMs = ms;
        emit telemetryUpdated();
    }

    if (settled) {
        m_animationActive = false;
        m_timer.stop();
        if (m_autoHide && targetAlpha > 0) {
            const int delayMs = std::max(1, int(std::ceil((kAutoHideDelaySec - m_stillSeconds) * 1000.0)));
            m_fadeAtNs = nowNs() + delayMs * 1'000'000LL;
            if (m_timing == Timing::Realtime) m_fadeDelay.start(delayMs);
        }
    }
}
