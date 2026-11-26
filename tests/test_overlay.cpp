#include "../src/motion_engine.h"
#include "../src/overlay_window.h"
#include "../src/theme.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char *message)
{
    if (condition) {
        std::cout << "PASS: " << message << '\n';
    } else {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void processFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

QImage grab(OverlayStrip &strip)
{
    processFor(35);
    return strip.screen()->grabWindow(strip.winId()).toImage().convertToFormat(QImage::Format_ARGB32);
}

int nonBlackPixels(const QImage &image)
{
    int pixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) pixels += (row[x] & 0x00ffffff) != 0;
    }
    return pixels;
}

struct Difference {
    int pixels = 0;
    int maximumChannel = 0;
};

Difference difference(const QImage &first, const QImage &second)
{
    if (first.size() != second.size()) return {-1, 255};
    Difference result;
    for (int y = 0; y < first.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(first.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            if (a[x] == b[x]) continue;
            ++result.pixels;
            result.maximumChannel = std::max({result.maximumChannel,
                std::abs(qRed(a[x]) - qRed(b[x])), std::abs(qGreen(a[x]) - qGreen(b[x])),
                std::abs(qBlue(a[x]) - qBlue(b[x])), std::abs(qAlpha(a[x]) - qAlpha(b[x]))});
        }
    }
    return result;
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    Theme theme;
    MotionEngine engine(MotionEngine::Timing::Manual);
    engine.setMode(QStringLiteral("sim"));
    engine.setSimPreset(QStringLiteral("mountain"));
    OverlayStrip left(OverlayStrip::Side::Left, &engine, &theme);
    OverlayStrip right(OverlayStrip::Side::Right, &engine, &theme);
    left.syncVisibility();
    right.syncVisibility();

    engine.advance(0.5);
    const QImage first = grab(left);
    engine.advance(0.5);
    const QImage moved = grab(left);
    expect(!first.isNull() && nonBlackPixels(first) > 0, "Offscreen strip paints visible simulation dots");
    expect(difference(first, moved).pixels > 0, "Incremental frame updates move the painted dots");

    engine.setDotSize(22);
    engine.setDotPairs(6);
    engine.setEdgeMargin(60);
    engine.advance(0.25);
    const QImage incremental = grab(left);
    left.update();
    const QImage full = grab(left);
    const Difference repaint = difference(incremental, full);
    expect(repaint.pixels >= 0 && repaint.pixels <= 2000 && repaint.maximumChannel <= 64,
           "Incremental repaint matches a full repaint without visible trails");

    left.resize(left.width(), std::max(120, left.height() / 2));
    engine.setEdgeMargin(24);
    engine.advance(0.25);
    const QImage resizedIncremental = grab(left);
    left.update();
    const Difference resized = difference(resizedIncremental, grab(left));
    expect(resized.pixels >= 0 && resized.pixels <= 2000 && resized.maximumChannel <= 64,
           "A shorter strip remains consistent after dirty-region clipping");

    engine.setSimPreset(QStringLiteral("manual"));
    engine.advance(15.0);
    const QImage fadedLeft = grab(left);
    const QImage fadedRight = grab(right);
    expect(nonBlackPixels(fadedLeft) == 0 && nonBlackPixels(fadedRight) == 0
               && !engine.isAnimationTimerActive() && engine.currentAlpha() == 0.0,
           "Settled strips fully clear after fade-out");

    if (failures) {
        std::cerr << "\nFAILED: " << failures << " overlay assertion(s)\n";
        return 1;
    }
    std::cout << "\nALL OVERLAY TESTS PASSED\n";
    return 0;
}
