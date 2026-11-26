package dev.omarchy.steadysender;

/**
 * A discover reply counts only when it answers the probe that is still
 * outstanding and it came from the target address and port.
 */
public final class ProbeAccept {
    private ProbeAccept() {}

    public static boolean allow(boolean awaiting, String targetIp, int targetPort,
                                String fromIp, int fromPort, String body) {
        if (!awaiting) return false;
        if (targetPort < 1 || targetPort > 65535 || fromPort != targetPort) return false;
        if (!sameIp(targetIp, fromIp)) return false;
        return ProbeReply.isSteady(body);
    }

    static boolean sameIp(String targetIp, String fromIp) {
        String a = HostCheck.normalizeAddress(targetIp);
        String b = HostCheck.normalizeAddress(fromIp);
        if (a == null || b == null) return false;
        return a.equalsIgnoreCase(b);
    }
}
