#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class QSettings;

class MotionEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool overlayEnabled READ overlayEnabled WRITE setOverlayEnabled NOTIFY overlayEnabledChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString simPreset READ simPreset WRITE setSimPreset NOTIFY simPresetChanged)
    Q_PROPERTY(bool simulationPaused READ simulationPaused NOTIFY simulationPausedChanged)
    Q_PROPERTY(int dotPairs READ dotPairs WRITE setDotPairs NOTIFY visualsChanged)
    Q_PROPERTY(double dotSize READ dotSize WRITE setDotSize NOTIFY visualsChanged)
    Q_PROPERTY(double opacity READ opacity WRITE setOpacity NOTIFY visualsChanged)
    Q_PROPERTY(double sensitivity READ sensitivity WRITE setSensitivity NOTIFY visualsChanged)
    Q_PROPERTY(double edgeMargin READ edgeMargin WRITE setEdgeMargin NOTIFY visualsChanged)
    Q_PROPERTY(bool autoHide READ autoHide WRITE setAutoHide NOTIFY visualsChanged)
    Q_PROPERTY(bool useAccentColor READ useAccentColor WRITE setUseAccentColor NOTIFY visualsChanged)

    // Throttled (~4 Hz) telemetry properties for QML UI to avoid 40-60 Hz SceneGraph repaints
    Q_PROPERTY(double ax READ ax NOTIFY telemetryUpdated)
    Q_PROPERTY(double ay READ ay NOTIFY telemetryUpdated)
    Q_PROPERTY(double offsetX READ offsetX NOTIFY telemetryUpdated)
    Q_PROPERTY(double offsetY READ offsetY NOTIFY telemetryUpdated)
    Q_PROPERTY(double scrollY READ scrollY NOTIFY frameUpdated)
    Q_PROPERTY(double currentAlpha READ currentAlpha NOTIFY frameUpdated)

    Q_PROPERTY(int imuHz READ imuHz NOTIFY imuStatusChanged)
    Q_PROPERTY(bool phoneConnected READ phoneConnected NOTIFY imuStatusChanged)
    Q_PROPERTY(QString imuSource READ imuSource NOTIFY imuStatusChanged)
    Q_PROPERTY(QString calibrationHint READ calibrationHint NOTIFY imuStatusChanged)

public:
    enum class Timing { Realtime, Manual };
    enum class SimulationTimeout { Enabled, Disabled };
    explicit MotionEngine(Timing timing = Timing::Realtime, QObject *parent = nullptr);
    MotionEngine(Timing timing, SimulationTimeout timeout, QObject *parent = nullptr);

    // In manual mode this also advances the idle and phone watchdog clocks.
    void advance(double seconds);

    struct Acceleration { double x; double y; };
    static Acceleration simulationSample(const QString &preset, double seconds);
    static bool validPreset(const QString &preset);

    bool overlayEnabled() const { return m_overlayEnabled; }
    void setOverlayEnabled(bool v);

    QString mode() const { return m_mode; }
    void setMode(const QString &m);

    QString simPreset() const { return m_simPreset; }
    void setSimPreset(const QString &p);
    bool simulationPaused() const { return m_simulationPaused; }
    void noteInteraction();
    Q_INVOKABLE void resumeSimulation();

    int dotPairs() const { return m_dotPairs; }
    void setDotPairs(int v);

    double dotSize() const { return m_dotSize; }
    void setDotSize(double v);

    double opacity() const { return m_opacity; }
    void setOpacity(double v);

    double sensitivity() const { return m_sensitivity; }
    void setSensitivity(double v);

    double edgeMargin() const { return m_edgeMargin; }
    void setEdgeMargin(double v);

    bool autoHide() const { return m_autoHide; }
    void setAutoHide(bool v);

    bool useAccentColor() const { return m_useAccentColor; }
    void setUseAccentColor(bool v);

    double ax() const { return m_filtAx; }
    double ay() const { return m_filtAy; }
    double offsetX() const { return m_offsetX; }
    double offsetY() const { return m_offsetY; }
    double scrollY() const { return m_scrollY; }
    double currentAlpha() const { return m_currentAlpha; }

    int imuHz() const { return m_imuHz; }
    bool phoneConnected() const { return m_phoneConnected; }
    QString imuSource() const { return m_imuSource; }
    QString calibrationHint() const { return m_calibrationHint; }

    static constexpr double minimumVisibleOpacity = 0.01;

    bool isAnimationTimerActive() const { return m_animationActive; }

    Q_INVOKABLE void triggerImpulse(double gx, double gy);
    Q_INVOKABLE void calibrate();
    Q_INVOKABLE void resetDefaults();
    void loadSettings(const QSettings &settings);
    void saveSettings(QSettings &settings) const;

    bool ingestImu(double ax, double ay, double az, double gx, double gy, double gz,
                   bool gravityIncluded, const QString &source, bool idle = false);
    void disconnectImu();
    void resetImuCalibration();

signals:
    void overlayEnabledChanged();
    void modeChanged();
    void simPresetChanged();
    void simulationPausedChanged();
    void visualsChanged();
    void frameUpdated();
    void telemetryUpdated();
    void imuStatusChanged();

private:
    void step();
    void animate(double dt);
    void beginFade();
    void cancelFadeDelay();
    qint64 nowNs() const;
    qint64 nowMs() const { return nowNs() / 1'000'000; }
    void checkImuWatchdog();
    void wakeAnimationTimer();
    void updateTimerState();
    bool isContinuousSim() const;
    void updateSimulationTimeout();
    void pauseSimulation();

    const Timing m_timing;
    const SimulationTimeout m_simulationTimeout;
    QTimer m_timer;
    QTimer m_fadeDelay;
    QTimer m_watchdogTimer;
    QTimer m_simTimeout;
    QElapsedTimer m_clock;
    bool m_animationActive = false;
    qint64 m_manualNs = 0;
    qint64 m_fadeAtNs = -1;
    qint64 m_lastHzMs = 0;
    qint64 m_lastTelemetryMs = 0;
    qint64 m_lastStepNs = 0;
    qint64 m_lastImuMs = -1;
    qint64 m_simDeadlineMs = -1;
    double m_simTime = 0.0;

    bool m_overlayEnabled = true;
    QString m_mode = QStringLiteral("phone");
    QString m_simPreset = QStringLiteral("city");
    bool m_simulationPaused = false;

    int m_dotPairs = 4;
    double m_dotSize = 12.0;
    double m_opacity = 0.80;
    double m_sensitivity = 1.0;
    double m_edgeMargin = 26.0;
    bool m_autoHide = true;
    bool m_useAccentColor = false;

    // Gravity estimation & calibration for phone IMU
    bool m_hasGravity = false;
    bool m_lastGravityIncluded = false;
    double m_lastImuX = 0.0, m_lastImuY = 0.0, m_lastImuZ = 0.0;
    double m_gravX = 0.0, m_gravY = 0.0, m_gravZ = 9.81;
    double m_biasX = 0.0, m_biasY = 0.0;
    double m_rawLinX = 0.0, m_rawLinY = 0.0;
    double m_rawGyroZ = 0.0;

    // Manual impulse accumulator
    double m_impX = 0.0, m_impY = 0.0;

    // Filtered signals & visual kinematics
    double m_filtAx = 0.0, m_filtAy = 0.0;
    double m_offsetX = 0.0, m_offsetY = 0.0;
    double m_velX = 0.0, m_velY = 0.0;
    double m_scrollY = 0.0;
    double m_stillSeconds = 0.0;
    double m_currentAlpha = 0.0;

    // Last emitted visual values to avoid redundant Wayland SHM commits
    double m_lastEmitX = -999.0;
    double m_lastEmitY = -999.0;
    double m_lastEmitScroll = -999.0;
    double m_lastEmitAlpha = -1.0;

    int m_packetCount = 0;
    int m_imuHz = 0;
    bool m_phoneConnected = false;
    QString m_imuSource = QStringLiteral("等待手機訊號...");
    QString m_calibrationHint = QStringLiteral("尚未校正");
};
