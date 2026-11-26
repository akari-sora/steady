package dev.omarchy.steadysender;

/** Applies {@link ProbeAccept} and only then lets the probe count a reply. */
public final class ReplyIntake {
    private ReplyIntake() {}

    public static boolean take(DesktopProbe probe, long nowMs,
                               String targetIp, int targetPort,
                               String fromIp, int fromPort, String body) {
        if (probe == null) return false;
        if (!ProbeAccept.allow(probe.awaitingReply(), targetIp, targetPort, fromIp, fromPort, body)) {
            return false;
        }
        probe.markReply(nowMs);
        return true;
    }
}
