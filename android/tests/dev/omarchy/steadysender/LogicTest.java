package dev.omarchy.steadysender;

/**
 * Plain JVM checks. The numeric asserts are literals on purpose: editing a
 * threshold or timeout in the production classes fails this file.
 */
public final class LogicTest {
    private static int failed;
    private static int passed;

    public static void main(String[] args) {
        thresholds();
        stationary();
        driveWindow();
        probe();
        discovery();
        deepLink();
        wire();
        servicePath();
        if (failed > 0) {
            System.out.println(failed + " FAILED, " + passed + " passed");
            System.exit(1);
        }
        System.out.println("OK " + passed);
    }

    /** 5, 25, 50, 100 and 200 Hz, as milliseconds between samples. */
    private static final long[] RATE_MS = {200L, 40L, 20L, 10L, 5L};

    private static void thresholds() {
        eqD(StationaryDetector.ACCEL_THRESHOLD, 0.22, "accel threshold");
        eqD(StationaryDetector.GYRO_THRESHOLD, 0.08, "gyro threshold");
        eqD(StationaryDetector.GRAVITY_KEEP, 0.85, "gravity keep");
        eqD(StationaryDetector.GRAVITY_BLEND, 0.15, "gravity blend");
        check(StationaryDetector.GRAVITY_KEEP + StationaryDetector.GRAVITY_BLEND == 1.0, "gravity blend sums to 1");
        eqL(StreamTiming.IDLE_ENTER_MS, 4_000L, "idle enter");
        eqL(StreamTiming.AUTO_STOP_STILL_MS, 1_200_000L, "still autostop");
        eqL(StreamTiming.clampStillStop(0L), 1_200_000L, "still override absent");
        eqL(StreamTiming.clampStillStop(-5L), 1_200_000L, "still override negative");
        eqL(StreamTiming.clampStillStop(2_000_000L), 1_200_000L, "still override cannot lengthen");
        eqL(StreamTiming.clampStillStop(1_200_000L), 1_200_000L, "still override equal stays");
        eqL(StreamTiming.clampStillStop(8_000L), 8_000L, "still override can shorten");
        eqI(StreamTiming.SAMPLE_HZ, 25, "sample hz");
        eqI(StreamTiming.SAMPLE_PERIOD_US, 40_000, "sample period");
        eqD(StreamTiming.DRIVE_SEND_HZ, 12.5, "drive send hz");
        eqL(StreamTiming.DRIVE_SEND_INTERVAL_MS, 80L, "drive interval");
        eqI(StreamTiming.DRIVE_REPORT_LATENCY_US, 80_000, "drive batch");
        eqI(StreamTiming.DRIVE_AVERAGE_COUNT, 2, "pair size");
        eqI(StreamTiming.IDLE_SAMPLE_PERIOD_US, 100_000, "idle period");
        eqI(StreamTiming.IDLE_REPORT_LATENCY_US, 0, "idle batch");
        eqI(StreamTiming.BIND_FALLBACK_LATENCY_US, 0, "bind retry latency");
        eqL(StreamTiming.IDLE_SEND_INTERVAL_MS, 1_000L, "idle send");
        check(StreamTiming.IDLE_SAMPLE_PERIOD_US + StreamTiming.IDLE_REPORT_LATENCY_US <= 100_000,
            "idle delivery budget");
        eqL(DesktopProbe.REACHABLE_INTERVAL_MS, 10_000L, "probe interval");
        eqL(DesktopProbe.UNREACHABLE_INTERVAL_MS, 1_000L, "unreachable probe");
        eqI(DesktopProbe.MISSES_TO_PAUSE, 3, "misses");
        eqL(DesktopProbe.UNREACHABLE_STOP_MS, 300_000L, "unreachable stop");
        check(Math.abs(1000.0 / StreamTiming.DRIVE_SEND_HZ - StreamTiming.DRIVE_SEND_INTERVAL_MS) < 0.001,
            "interval derived from rate");
        check(thresholdStill(0.22, 0, 0) == 4_000L, "0.22 is not motion");
        check(thresholdStill(0.2200001, 0, 0) == 0L, "just over 0.22 is motion");
        check(thresholdStill(0, -0.22, 0) == 4_000L, "ay -0.22 is not motion");
        check(thresholdStill(0, -0.2200001, 0) == 0L, "ay just over 0.22 is motion");
        check(thresholdStill(0, 0, 0.08) == 4_000L, "0.08 rad/s is not motion");
        check(thresholdStill(0, 0, -0.0800001) == 0L, "just over 0.08 rad/s is motion");
        check(thresholdStillAz(5.0) == 4_000L, "linear path ignores az");
    }

    /** Stillness at t=4s for one held linear sample. Motion on that sample returns 0. */
    private static long thresholdStill(double ax, double ay, double gz) {
        StationaryDetector d = new StationaryDetector();
        d.reset(0L);
        return d.observe(4_000L, ax, ay, 0, gz);
    }

    private static long thresholdStillAz(double az) {
        StationaryDetector d = new StationaryDetector();
        d.reset(0L);
        return d.observe(4_000L, 0, 0, az, 0);
    }

    private static void stationary() {
        biasAtRates();
        stepsFromStationary();
        sustainedNeverIdle();
        stopAndGoNeverIdle();
        shortBrakeThenSustained();
        gravityPath();
        nonFiniteDoesNotStick();
        landscapeStep();
    }

    /** ±0.03 around the bias. Peak |noise| is the amplitude, so 0.15 stays under 0.22 and 0.30 does not. */
    private static double biasNoise(long tMs, int axis) {
        return 0.03 * Math.sin(tMs * 0.017 + axis * 1.7);
    }

    private static void biasAtRates() {
        for (long period : RATE_MS) {
            StationaryDetector quiet = new StationaryDetector();
            quiet.reset(0L);
            boolean early = false;
            long atFour = -1L;
            for (long t = 0; t <= 8_000; t += period) {
                long still = quiet.observe(t, 0.15 + biasNoise(t, 0), biasNoise(t, 1), 0, 0);
                if (t < 4_000 && still >= 4_000L) early = true;
                if (t >= 4_000 && atFour < 0) atFour = still;
            }
            check(!early && atFour >= 4_000L, "bias 0.15 stationary after 4s at " + period + " ms, still=" + atFour);

            StationaryDetector hot = new StationaryDetector();
            hot.reset(0L);
            boolean idled = false;
            for (long t = 0; t <= 20_000; t += period) {
                long still = hot.observe(t, 0.30 + biasNoise(t, 0), biasNoise(t, 1), 0, 0);
                if (still >= 4_000L) idled = true;
            }
            check(!idled, "bias 0.30 never stationary at " + period + " ms");

            StationaryDetector g950 = new StationaryDetector();
            g950.reset(0L);
            boolean g950Idle = false;
            for (long t = 0; t <= 20_000; t += period) {
                long still = g950.observe(t, 0.545, 0.207, 0, 0);
                if (still >= 4_000L) g950Idle = true;
            }
            check(!g950Idle, "G950 rest level stays moving at " + period + " ms");
        }
    }

    private static void stepsFromStationary() {
        double[] steps = {0.30, 1.0, 2.5};
        for (double step : steps) {
            for (long period : RATE_MS) {
                StationaryDetector d = new StationaryDetector();
                d.reset(0L);
                long still = 0L;
                for (long t = 0; t <= 4_000; t += period) {
                    still = d.observe(t, 0.15 + biasNoise(t, 0), biasNoise(t, 1), 0, 0);
                }
                check(still >= 4_000L, "idle before step " + step + " at " + period);
                long moved = d.observe(4_000L + period, step, 0, 0, 0);
                check(moved == 0L, "first " + step + " sample is motion at " + period + " ms, still=" + moved);
            }
        }
        StationaryDetector ay = new StationaryDetector();
        ay.reset(0L);
        long still = 0L;
        for (long t = 0; t <= 4_000; t += 40) still = ay.observe(t, 0, 0, 0, 0);
        check(still >= 4_000L, "idle before ay step");
        check(ay.observe(4_040L, 0, -0.30, 0, 0) == 0L, "first ay sample over the threshold is motion");
    }

    /**
     * Sequences the learned detector reported as idle: a pull-away, a long
     * brake, a long coast, and a gentle turn. None of them may produce an
     * idle packet while the input is still present.
     */
    private static void sustainedNeverIdle() {
        for (long period : RATE_MS) {
            StreamOutput pull = new StreamOutput();
            pull.reset(0L);
            check(!reportedIdle(pull, 0L, 1_000L - period, period, 0, 0),
                "first second before +1.5 is not idle at " + period);
            check(!reportedIdle(pull, 1_000L, 31_000L, period, 1.5, 0),
                "+1.5 for 30s from 1s is not idle at " + period);

            StreamOutput coast = restUntilIdle(period);
            long from = 5_000L + period;
            check(!reportedIdle(coast, from, from + 60_000L, period, -0.4, 0),
                "-0.4 for 60s from stationary is not idle at " + period);

            StreamOutput brake = restUntilIdle(period);
            check(!reportedIdle(brake, from, from + 25_000L, period, -1.3, 0),
                "-1.3 for 25s from stationary is not idle at " + period);

            StreamOutput turn = restUntilIdle(period);
            check(!reportedIdle(turn, from, from + 60_000L, period, 0, 0.10),
                "0.1 rad/s turn for 60s from stationary is not idle at " + period);

            StreamOutput hard = restUntilIdle(period);
            check(!reportedIdle(hard, from, from + 25_000L, period, 1.0, 0),
                "+1.0 for 25s from stationary is not idle at " + period);
        }
    }

    /** ±0.6 m/s², 7 s period, from the moment streaming starts. Opus round 1. */
    private static double stopGoAx(long tMs) {
        return 0.6 * Math.sin(2.0 * Math.PI * (double) tMs / 7000.0);
    }

    private static void stopAndGoNeverIdle() {
        for (long period : RATE_MS) {
            StreamOutput out = new StreamOutput();
            out.reset(0L);
            boolean idle = false;
            for (long t = 0; t <= 60_000; t += period) {
                StreamOutput.Out d = out.onSample(
                    t, stopGoAx(t), 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
                if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
                if (d.motionIdle || d.idle || d.kind == StreamOutput.Kind.STOP_STILL) idle = true;
            }
            check(!idle, "stop-and-go 60s is not idle at " + period + " ms");
        }
    }

    /** One second at +0.30, back to rest, then +0.30 held for 25 s. */
    private static void shortBrakeThenSustained() {
        for (long period : RATE_MS) {
            StreamOutput out = restUntilIdle(period);
            long brake = 5_000L + period;
            check(!reportedIdle(out, brake, brake + 1_000L, period, 0.30, 0),
                "1s brake is not idle at " + period);
            long back = brake + 1_000L + period;
            boolean rested = false;
            for (long t = back; t <= back + 6_000L; t += period) {
                StreamOutput.Out d = out.onSample(t, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
                if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
                if (d.motionIdle) rested = true;
            }
            check(rested, "rest after the short brake at " + period);
            long hold = back + 6_000L + period;
            check(!reportedIdle(out, hold, hold + 25_000L, period, 0.30, 0),
                "sustained 0.30 after a short brake is not idle at " + period);
        }
    }

    private static void gravityPath() {
        for (long period : RATE_MS) {
            StationaryDetector d = new StationaryDetector();
            d.setGravityIncluded(true);
            d.reset(0L);
            long still = d.observe(0L, 0.40, -0.20, 9.81, 0);
            check(still == 0L, "gravity seeds at " + period);
            for (long t = period; t <= 4_000; t += period) {
                still = d.observe(t, 0.40, -0.20, 9.81, 0);
            }
            check(still >= 4_000L, "constant gravity stationary at " + period + " still=" + still);
            still = d.observe(4_000L + period, 0.70, -0.20, 9.81, 0);
            check(still == 0L, "gravity step of 0.30 is motion at " + period);
        }
    }

    /** One bad sample must not move the clock or the gravity average. */
    private static void nonFiniteDoesNotStick() {
        StationaryDetector d = new StationaryDetector();
        d.reset(0L);
        long still = 0L;
        for (long t = 0; t <= 2_000; t += 40) still = d.observe(t, 0.10, -0.05, 0.02, 0);
        check(d.observe(2_040L, Double.NaN, 0, 0, 0) == still, "NaN does not advance stillness");
        check(d.observe(2_080L, Double.POSITIVE_INFINITY, 0, 0, 0) == still, "+inf does not advance");
        check(d.observe(2_120L, 0, Double.NEGATIVE_INFINITY, 0, 0) == still, "-inf does not advance");
        check(d.observe(2_160L, 0, 0, 0, Double.NaN) == still, "bad yaw does not advance");
        check(d.observe(2_200L, 0.10, -0.05, 0.02, 0) == 2_200L, "good sample after NaN keeps the clock");
        check(d.observe(2_240L, 1.0, 0, 0, 0) == 0L, "step after NaN is motion");

        StationaryDetector g = new StationaryDetector();
        g.setGravityIncluded(true);
        g.reset(0L);
        g.observe(0L, 0.20, 9.80, 0.10, 0);
        for (long t = 40; t <= 1_000; t += 40) g.observe(t, 0.20, 9.80, 0.10, 0);
        check(g.observe(1_040L, Double.NaN, 9.80, 0.10, 0) == 1_000L, "gravity NaN freezes stillness");
        check(g.observe(1_080L, 0.20, 9.80, 0.10, 0) == 1_080L, "NaN did not move the gravity average");
    }

    private static void landscapeStep() {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        boolean packet = false;
        for (long t = 0; t <= 5_000; t += 40) {
            StreamOutput.Out d = out.onSample(t, 0, 0, 0, 0, 0, true, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
            if (d.idle) packet = true;
        }
        check(packet, "landscape rest idles");
        StreamOutput.Out step = out.onSample(5_040L, 0, -0.30, 0, 0, 0, true, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(step.kind == StreamOutput.Kind.EMIT && !step.idle && !step.motionIdle,
            "landscape lateral step leaves idle on that sample");
    }

    /** True when any sample in the range is treated or sent as stationary. */
    private static boolean reportedIdle(StreamOutput out, long start, long end, long period, double ax, double gz) {
        if (start > end) return false;
        for (long t = start; t <= end; t += period) {
            StreamOutput.Out d = out.onSample(t, ax, 0, 0, gz, gz, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
            if (d.motionIdle || d.idle || d.kind == StreamOutput.Kind.STOP_STILL) return true;
        }
        return false;
    }

    /** Zeros through 5 s. Mode is on at 4 s; the idle packet follows within one keepalive. */
    private static StreamOutput restUntilIdle(long period) {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        boolean mode = false;
        boolean packet = false;
        for (long t = 0; t <= 5_000; t += period) {
            StreamOutput.Out d = out.onSample(t, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
            if (t >= 4_000 && d.motionIdle) mode = true;
            if (d.idle) packet = true;
        }
        check(mode, "stationary mode by 4s at " + period + " ms");
        check(packet, "idle packet by 5s at " + period + " ms");
        return out;
    }

    private static void driveWindow() {
        StreamTiming.DriveWindow w = new StreamTiming.DriveWindow();
        w.reset(0L);
        check(!w.onSample(0L, 1, 0, 0, 0), "first sample waits");
        check(w.onSample(40L, 3, 0, 0, 0), "pair emits");
        eqD(w.emitAx(), 2.0, "pair average");

        int emits = 0;
        StreamTiming.DriveWindow rate = new StreamTiming.DriveWindow();
        rate.reset(0L);
        for (int t = 0; t <= 1000; t += 40) {
            if (rate.onSample(t, 1, 2, 3, 0.1)) emits++;
        }
        eqI(emits, 13, "12.5 Hz over 1s of 25 Hz samples");

        StreamTiming.DriveWindow held = new StreamTiming.DriveWindow();
        held.reset(0L);
        check(!held.onSample(0L, 0, 0, 0, 0), "seed");
        check(held.onSample(40L, 0, 0, 0, 0), "seed emit");
        check(!held.onSample(50L, 10, 0, 0, 0), "next first");
        check(!held.onSample(60L, 10, 0, 0, 0), "pair frozen inside 80ms");
        check(held.onSample(120L, 99, 0, 0, 0), "flush frozen pair");
        eqD(held.emitAx(), 10.0, "frozen pair excludes the newer sample");
        check(!held.onSample(120L, 99, 0, 0, 0), "newer sample starts the next pair");
        check(held.onSample(200L, 1, 0, 0, 0), "next pair");
        eqD(held.emitAx(), 99.0, "following pair is the two 99s");
    }

    private static void probe() {
        DesktopProbe p = new DesktopProbe();
        p.markSent(0L);
        check(p.samplesEnabled(), "samples flow before any miss");
        p.markSent(10_000L);
        p.markSent(20_000L);
        check(p.samplesEnabled(), "two misses still send");
        eqI(p.misses(), 2, "two misses");
        p.markSent(30_000L);
        check(!p.samplesEnabled(), "third miss pauses samples");
        eqI(p.misses(), 3, "three misses");
        check(!p.due(30_000L), "not due at the send");
        check(!p.due(30_999L), "unreachable probe is not faster than 1 Hz");
        check(p.due(31_000L), "unreachable probe is due at 1s");
        check(!p.shouldStop(30_000L + 299_999L), "stop waits 5 min");
        check(p.shouldStop(30_000L + 300_000L), "stop at 5 min");

        p.markReply(31_500L);
        check(p.samplesEnabled(), "reply resumes samples");
        eqI(p.misses(), 0, "reply clears misses");
        check(!p.shouldStop(9_999_999L), "reply cancels the stop");
        check(!p.due(31_500L), "reply rearms the 10s probe");
        check(p.due(41_500L), "next probe 10s after the reply");

        DesktopProbe again = new DesktopProbe();
        again.markSent(0L);
        again.markReply(50L);
        again.markSent(10_000L);
        eqI(again.misses(), 0, "a reply before the next probe is not a miss");

        DesktopProbe shorten = new DesktopProbe();
        shorten.overrideStopAfterMs(600_000L);
        eqL(shorten.stopAfterMs(), 300_000L, "override cannot lengthen");
        shorten.overrideStopAfterMs(20_000L);
        eqL(shorten.stopAfterMs(), 20_000L, "override can shorten");
        shorten.markSent(0L);
        shorten.markSent(10_000L);
        shorten.markSent(20_000L);
        shorten.markSent(30_000L);
        check(!shorten.shouldStop(30_000L + 19_999L), "short stop not early");
        check(shorten.shouldStop(30_000L + 20_000L), "short stop fires");
    }

    private static void discovery() {
        eqS(DiscoveryChoice.target("192.168.1.20", "10.1.1.1"), "192.168.1.20", "source wins");
        eqS(DiscoveryChoice.target("192.168.1.20", "198.51.100.8"), "192.168.1.20", "stale lan ignored");
        eqS(DiscoveryChoice.target("0.0.0.0", "10.1.1.1"), "10.1.1.1", "wildcard falls back");
        eqS(DiscoveryChoice.target(null, "10.1.1.1"), "10.1.1.1", "missing source falls back");
        eqS(DiscoveryChoice.target("127.0.0.1", "10.0.0.8"), "10.0.0.8", "loopback falls back");
        eqS(DiscoveryChoice.target("fe80::1%wlan0", "10.9.9.9"), "fe80::1", "zone id stripped");
        check(DiscoveryChoice.target("0.0.0.0", "not a host") == null, "both unusable");
        check(DiscoveryChoice.target("", "") == null, "empty");
    }

    private static void deepLink() {
        DeepLinkRequest ok = DeepLinkRequest.parse("steady", "connect", "10.9.9.9", "9999", null, null);
        check(ok != null && ok.ok(), "valid link");
        eqS(ok.host, "10.9.9.9", "link host");
        eqI(ok.port, 9999, "link port");

        DeepLinkRequest name = DeepLinkRequest.parse("steady", "connect", "laptop.local", "8766", null, null);
        check(name != null && name.ok(), "hostname");
        eqS(name.host, "laptop.local", "hostname value");

        DeepLinkRequest badPort = DeepLinkRequest.parse("steady", "connect", "10.9.9.9", "99999", null, null);
        check(badPort != null && !badPort.ok(), "99999 rejected");
        eqS(badPort.error, "bad port", "bad port error");
        check(badPort.host == null, "bad port does not carry a host to apply");

        for (String bad : new String[] {"0", "65536", "-1", "abc", "8766x", ""}) {
            DeepLinkRequest r = DeepLinkRequest.parse("steady", "connect", "10.0.0.1", bad, null, null);
            check(r != null && "bad port".equals(r.error), "rejected port " + bad);
        }
        DeepLinkRequest edge = DeepLinkRequest.parse("steady", "connect", "10.0.0.1", "65535", null, null);
        check(edge != null && edge.ok() && edge.port == 65535, "65535 accepted");

        DeepLinkRequest badHost = DeepLinkRequest.parse("steady", "connect", "10.9.9.999", "8766", null, null);
        check(badHost != null && "bad host".equals(badHost.error), "bad ipv4");
        DeepLinkRequest url = DeepLinkRequest.parse("steady", "connect", "http://evil.com", "8766", null, null);
        check(url != null && "bad host".equals(url.error), "url rejected");
        DeepLinkRequest bang = DeepLinkRequest.parse("steady", "connect", "not a host!", "8766", null, null);
        check(bang != null && "bad host".equals(bang.error), "punctuation rejected");
        DeepLinkRequest loop = DeepLinkRequest.parse("steady", "connect", "127.0.0.1", "8766", null, null);
        check(loop != null && "bad host".equals(loop.error), "loopback rejected");

        check(DeepLinkRequest.parse("https", "connect", "10.0.0.1", "1", null, null) == null, "wrong scheme");
        check(DeepLinkRequest.parse("steady", "other", "10.0.0.1", "1", null, null) == null, "wrong uri host");

        DeepLinkRequest portOnly = DeepLinkRequest.parse("steady", "connect", null, "8766", null, null);
        check(portOnly != null && portOnly.ok() && portOnly.host == null && portOnly.port == 8766, "port only");
        check(portOnly.applyAllowed("10.0.0.8", "1", "", ""), "port only is disclosed");

        DeepLinkRequest chips = DeepLinkRequest.parse("steady", "connect", null, null, "10.1.1.1", "10.2.2.2");
        check(chips != null && chips.ok(), "lan/ts link parses");
        String shown = chips.confirmMessage("192.168.1.9", "8766", "", "");
        check(shown.contains("lan: ") && shown.contains("→ 10.1.1.1"), "dialog names lan");
        check(shown.contains("ts: ") && shown.contains("→ 10.2.2.2"), "dialog names ts");
        check(chips.applyAllowed("192.168.1.9", "8766", "", ""), "lan/ts apply is disclosed");
        check(!chips.disclosed("要將目標改為 192.168.1.9:8766 嗎？"), "host sentence hides lan/ts");

        DeepLinkRequest both = DeepLinkRequest.parse("steady", "connect", "10.9.9.9", "9999", "10.1.1.1", "10.2.2.2");
        String full = both.confirmMessage("1.1.1.1", "8766", "10.0.0.1", "100.1.1.1");
        check(both.disclosed(full), "host+chips disclosed");
        check(full.contains("host: ") && full.contains("→ 10.9.9.9"), "host line");
        check(full.contains("→ 10.1.1.1") && full.contains("→ 10.2.2.2"), "chip lines");

        DeepLinkRequest opaque = DeepLinkRequest.parse(
            false, "steady", null, "10.0.0.2", "8766", null, null, true);
        check(opaque != null && !opaque.ok(), "opaque steady uri rejected");
        eqS(opaque.error, "bad uri", "opaque error");
        check(opaque.host == null && opaque.port < 0, "opaque carries nothing to apply");

        DeepLinkRequest thrown = DeepLinkRequest.parse(
            true, "steady", "connect", "10.0.0.2", "8766", "10.1.1.1", null, true);
        check(thrown != null && !thrown.ok() && thrown.host == null, "unreadable query rejected");
        eqS(thrown.error, "bad uri", "unreadable query error");

        check(DeepLinkRequest.parse(false, "https", "connect", "10.0.0.2", "1", null, null, true) == null,
            "opaque other scheme ignored");
        DeepLinkRequest kept = DeepLinkRequest.parse(
            true, "steady", "connect", "10.9.9.9", "9999", null, null, false);
        check(kept != null && kept.ok() && "10.9.9.9".equals(kept.host) && kept.port == 9999,
            "hierarchical link still applies");
    }

    private static void wire() {
        byte[] buf = new byte[192];
        StringBuilder sb = new StringBuilder();
        int n = SampleJson.encode(buf, sb, 0.5, -0.25, 0.125, -0.5, false, false);
        String driving = new String(buf, 0, n, java.nio.charset.StandardCharsets.US_ASCII);
        eqS(driving,
            "{\"ax\":0.500,\"ay\":-0.250,\"az\":0.125,\"gz\":-0.500,\"grav\":false,\"src\":\"Steady Android\"}",
            "driving json");
        check(!driving.contains("idle"), "driving omits idle");

        n = SampleJson.encode(buf, sb, 0.5, 0, 0, 0, true, true);
        String idle = new String(buf, 0, n, java.nio.charset.StandardCharsets.US_ASCII);
        eqS(idle,
            "{\"ax\":0.500,\"ay\":0.000,\"az\":0.000,\"gz\":0.000,\"grav\":true,\"idle\":true,\"src\":\"Steady Android\"}",
            "idle json keeps the sample");
        check(idle.contains("\"idle\":true"), "idle flag");
        check(idle.contains("\"grav\":true"), "grav kept");
        check(idle.contains("\"src\":\"Steady Android\""), "src kept");

        check(ProbeReply.isSteady("{\"steady\":true,\"lan\":\"1.2.3.4\",\"ts\":\"\",\"udp\":8766}"), "steady true");
        check(ProbeReply.isSteady("{\"steady\": true}"), "steady spaced");
        check(!ProbeReply.isSteady("{\"steady\":false}"), "steady false");
        check(!ProbeReply.isSteady(driving), "sample is not a reply");
        check(!ProbeReply.isSteady(null), "null reply");
        check(ProbeReply.isSteady("{\"steady\":true,\"lan\":\"192.168.1.5\",\"ts\":\"\",\"udp\":8766}\n"),
            "desktop compact reply");
        check(!ProbeReply.isSteady("xx{\"steady\":true}"), "prefix junk");
        check(!ProbeReply.isSteady("{\"steady\":true} trailing"), "trailing junk");
        check(!ProbeReply.isSteady("{\"msg\":\"steady:true\"}"), "steady inside a string");
        check(!ProbeReply.isSteady("{\"steady\":\"true\"}"), "string true");
        check(!ProbeReply.isSteady(""), "empty body");
    }

    private static void servicePath() {
        leavingIdleEmitsCurrentSample();
        batchedSensorTimesReact();
        collapsedStampsDoNotReact();
        replySource();
        sensorBind();
        timeoutIgnoresIntent();
        wakeRenewAfterRelease();
        targetSwap();
        discoveryIsNotStored();
        badSamplesStop();
    }

    /** Deleting the leave-idle emit makes this fail: the first driving sample would wait for a pair. */
    private static void leavingIdleEmitsCurrentSample() {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        boolean sawIdle = false;
        for (long t = 0; t <= 8_000; t += 40) {
            StreamOutput.Out d = out.onSample(t, 0.05, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
            if (d.kind == StreamOutput.Kind.EMIT && d.idle) sawIdle = true;
        }
        check(sawIdle, "rest emits an idle keepalive");
        StreamOutput.Out step = out.onSample(8_040L, 2.5, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(step.kind == StreamOutput.Kind.EMIT, "leaving idle emits on that sample");
        check(!step.idle, "the leaving sample is not idle");
        eqD(step.ax, 2.5, "leaving sample is not averaged away");
        check(!step.wantIdleSensors, "leaving idle asks for the driving sensors");
    }

    /** Samples that arrive together still use their own sensor times. */
    private static void batchedSensorTimesReact() {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        long period = StreamTiming.IDLE_SAMPLE_PERIOD_US / 1000L;
        long t = 0L;
        for (; t <= 9_000; t += period) {
            StreamOutput.Out d = out.onSample(t, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
        }
        long restAt = t - period;
        long firstBrake = restAt + period;
        long movedAt = -1L;
        StreamOutput.Out hit = null;
        for (long s = firstBrake; s <= firstBrake + 400; s += period) {
            StreamOutput.Out d = out.onSample(s, 1.0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT && !d.idle) {
                movedAt = s;
                hit = d;
                break;
            }
        }
        long wall = movedAt < 0 ? -1L : movedAt - restAt;
        check(hit != null && wall <= 200L, "batched 1.0 reacts in " + wall + " ms");
        eqD(hit == null ? 0 : hit.ax, 1.0, "brake sample emitted whole");
        eqL(SensorClock.toElapsedMs(1_000_000_000L), 1_000L, "stamp 1s");
        eqL(SensorClock.toElapsedMs(1_100_000_000L), 1_100L, "stamp 1.1s");
        eqL(SensorClock.toElapsedMs(0L), -1L, "stamp 0 rejected");
        eqL(SensorClock.toElapsedMs(-5L), -1L, "negative stamp rejected");
    }

    /** The same values stamped at one instant do not look like a brake. */
    private static void collapsedStampsDoNotReact() {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        for (long t = 0; t <= 9_000; t += 100) {
            StreamOutput.Out d = out.onSample(t, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) out.noteSent(t);
        }
        StreamOutput.Out older = out.onSample(8_900L, 2.5, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(older.kind == StreamOutput.Kind.NONE, "older timestamp is ignored");
        check(older.motionIdle, "older timestamp does not leave idle");
        StreamOutput.Out same = out.onSample(9_000L, 1.0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(same.kind == StreamOutput.Kind.EMIT && !same.idle, "same timestamp over 0.22 is motion");
        check(!same.motionIdle, "same timestamp leaves idle");
    }

    private static void replySource() {
        String good = "{\"steady\":true,\"lan\":\"10.0.0.2\",\"ts\":\"\",\"udp\":8766}";
        DesktopProbe p = new DesktopProbe();
        p.markSent(0L);
        check(p.awaitingReply(), "probe is outstanding");
        check(!ReplyIntake.take(p, 50L, "10.0.0.2", 8766, "10.9.9.9", 8766, good), "wrong address");
        check(p.awaitingReply() && p.samplesEnabled(), "wrong address leaves the probe");
        check(!ReplyIntake.take(p, 60L, "10.0.0.2", 8766, "10.0.0.2", 9999, good), "wrong port");
        check(p.awaitingReply(), "wrong port still outstanding");
        check(!ReplyIntake.take(p, 70L, "10.0.0.2", 8766, "10.0.0.2", 8766, "not json"), "not json");
        DesktopProbe idle = new DesktopProbe();
        check(!ReplyIntake.take(idle, 10L, "10.0.0.2", 8766, "10.0.0.2", 8766, good), "unsolicited");
        check(idle.misses() == 0 && idle.samplesEnabled(), "unsolicited does not reset");
        for (int i = 0; i < 40; i++) {
            check(!ReplyIntake.take(p, 80L + i, "10.0.0.2", 8766, "198.51.100.8", 1, good), "flood " + i);
        }
        check(p.awaitingReply(), "flood left the probe outstanding");
        check(ReplyIntake.take(p, 200L, "10.0.0.2", 8766, "10.0.0.2", 8766, good), "matching reply");
        check(!p.awaitingReply() && p.samplesEnabled() && p.misses() == 0, "matching reply clears");
        check(!ReplyIntake.take(p, 210L, "10.0.0.2", 8766, "10.0.0.2", 8766, good), "second reply is unsolicited");

        DesktopProbe paused = new DesktopProbe();
        paused.markSent(0L);
        paused.markSent(10_000L);
        paused.markSent(20_000L);
        paused.markSent(30_000L);
        check(!paused.samplesEnabled(), "paused before the flood");
        for (int i = 0; i < 20; i++) {
            ReplyIntake.take(paused, 31_000L, "10.0.0.2", 8766, "10.9.9.9", 8766, good);
        }
        check(!paused.samplesEnabled(), "wrong-address flood does not resume");
        check(paused.shouldStop(30_000L + 300_000L), "flood does not move the stop");
    }

    private static void sensorBind() {
        SensorBind retry = SensorBind.after(false, true, false, true, false);
        check(retry.step == SensorBind.Step.RETRY && !retry.bound, "accel miss retries");
        SensorBind stop = SensorBind.after(false, true, false, true, true);
        check(stop.step == SensorBind.Step.STOP && !stop.bound, "accel miss after retry stops");
        check(stop.error != null && stop.error.contains("加速度計"), "stop names the accelerometer");
        SensorBind gyro = SensorBind.after(true, true, false, false, true);
        check(gyro.step == SensorBind.Step.BOUND && gyro.bound && !gyro.gyroOn, "gyro miss degrades");
        check(gyro.error != null && gyro.error.contains("陀螺儀"), "gyro miss is visible");
        check(!gyro.idleModeApplied, "driving mode stored only with the successful accel");
        SensorBind ok = SensorBind.after(true, true, true, true, false);
        check(ok.step == SensorBind.Step.BOUND && ok.bound && ok.gyroOn && ok.idleModeApplied && ok.error == null,
            "both sensors bound");
        SensorBind idle = SensorBind.after(true, false, false, true, false);
        check(idle.bound && idle.idleModeApplied && !idle.gyroOn && idle.error == null, "idle accel only");
    }

    private static void timeoutIgnoresIntent() {
        eqL(TimeoutSource.stillMs(1L, ""), 1_200_000L, "still extra ignored");
        eqL(TimeoutSource.stillMs(1L, null), 1_200_000L, "still property absent");
        eqL(TimeoutSource.stillMs(50L, "8000"), 8_000L, "still property shortens");
        eqL(TimeoutSource.stillMs(1L, "99999999"), 1_200_000L, "still property cannot lengthen");
        eqL(TimeoutSource.stillMs(1L, "nope"), 1_200_000L, "still property garbage");
        eqL(TimeoutSource.unreachableMs(1_000L, ""), 300_000L, "unreachable extra ignored");
        eqL(TimeoutSource.unreachableMs(1L, "20000"), 20_000L, "unreachable property shortens");
        eqL(TimeoutSource.unreachableMs(1L, "500"), 300_000L, "unreachable property too small");
    }

    private static void wakeRenewAfterRelease() {
        WakeGate g = new WakeGate();
        g.arm();
        check(g.isHeld(), "arm holds");
        boolean earlyPermit = true;
        g.release();
        check(!g.tryRenew(earlyPermit), "stale permit does not renew");
        check(!g.isHeld(), "release drops the lock");
        g.arm();
        check(g.tryRenew(true), "live session renews");
        check(g.isHeld(), "renew holds");
        check(!g.tryRenew(false), "stopped tick does not renew");
    }

    private static void targetSwap() {
        check(!TargetRefresh.shouldResolve(0, true, false), "quiet link keeps the address");
        check(TargetRefresh.shouldResolve(1, true, false), "a miss resolves again");
        check(TargetRefresh.shouldResolve(0, false, false), "unreachable resolves again");
        check(TargetRefresh.shouldResolve(0, true, true), "network change resolves again");
        TargetRefresh.View a = TargetRefresh.swap(null, "192.168.1.10", 8766);
        eqS(a.ip, "192.168.1.10", "first address");
        eqI(a.generation, 1, "first generation");
        TargetRefresh.View same = TargetRefresh.swap(a, "192.168.1.10", 8766);
        check(same == a, "same address keeps the view");
        TargetRefresh.View b = TargetRefresh.swap(a, "192.168.1.40", 8766);
        eqS(b.ip, "192.168.1.40", "new address");
        eqI(b.generation, 2, "generation bumps");
        DesktopProbe p = new DesktopProbe();
        p.markSent(0L);
        String body = "{\"steady\":true}";
        check(!ReplyIntake.take(p, 10L, b.ip, b.port, a.ip, a.port, body), "old address after swap");
        check(ReplyIntake.take(p, 20L, b.ip, b.port, b.ip, b.port, body), "new address after swap");
    }

    private static void discoveryIsNotStored() {
        DiscoveryCommit offer = DiscoveryCommit.offer("192.168.1.20", "100.64.0.2");
        check(offer != null && !offer.persist(), "first reply is not stored");
        eqS(offer.host, "192.168.1.20", "offer host");
        eqS(offer.ts, "100.64.0.2", "offer ts");
        DiscoveryCommit saved = offer.confirm();
        check(saved.persist() && !offer.persist(), "only the confirm is stored");
        check(DiscoveryCommit.offer("127.0.0.1", null) == null, "loopback offer dropped");
        check(DiscoveryCommit.offer(null, "10.0.0.1") == null, "missing host dropped");
        DiscoveryCommit noTs = DiscoveryCommit.offer("10.0.0.8", "not a host");
        check(noTs != null && noTs.ts == null && !noTs.persist(), "bad ts is not kept");
    }

    private static void badSamplesStop() {
        StreamOutput out = new StreamOutput();
        out.reset(0L);
        StreamOutput.Out last = null;
        for (int i = 0; i < StreamOutput.BAD_SAMPLES_TO_STOP; i++) {
            last = out.onSample(1_000L + i * 40L, Double.NaN, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        }
        check(last != null && last.kind == StreamOutput.Kind.STOP_SENSOR, "a run of NaN stops");
        check(last.error != null, "sensor stop has an error");
        StreamOutput one = new StreamOutput();
        one.reset(0L);
        for (long t = 0; t <= 6_000; t += 40) {
            StreamOutput.Out d = one.onSample(t, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
            if (d.kind == StreamOutput.Kind.EMIT) one.noteSent(t);
        }
        StreamOutput.Out skip = one.onSample(6_040L, Double.NaN, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(skip.kind == StreamOutput.Kind.NONE, "one NaN does not stop");
        StreamOutput.Out back = one.onSample(6_080L, 0, 0, 0, 0, 0, false, true, StreamTiming.AUTO_STOP_STILL_MS);
        check(back.kind != StreamOutput.Kind.STOP_SENSOR, "a good sample follows one NaN");
        check(back.motionIdle, "NaN did not leave idle");
    }

    private static void check(boolean cond, String msg) {
        if (!cond) {
            failed++;
            System.out.println("FAIL " + msg);
        } else {
            passed++;
        }
    }

    private static void eqI(int actual, int expected, String msg) {
        check(actual == expected, msg + " expected " + expected + " got " + actual);
    }

    private static void eqL(long actual, long expected, String msg) {
        check(actual == expected, msg + " expected " + expected + " got " + actual);
    }

    private static void eqD(double actual, double expected, String msg) {
        check(Double.compare(actual, expected) == 0, msg + " expected " + expected + " got " + actual);
    }

    private static void eqS(String actual, String expected, String msg) {
        check(expected.equals(actual), msg + " expected [" + expected + "] got [" + actual + "]");
    }
}
