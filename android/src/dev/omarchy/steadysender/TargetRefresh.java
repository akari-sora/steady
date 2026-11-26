package dev.omarchy.steadysender;

/**
 * Resolve the target hostname again after a miss or a network change.
 * A new address replaces the old one as a whole: replies are checked
 * against the address that is current after the swap.
 */
public final class TargetRefresh {
    private TargetRefresh() {}

    public static boolean shouldResolve(int misses, boolean samplesEnabled, boolean networkChanged) {
        return networkChanged || !samplesEnabled || misses > 0;
    }

    public static final class View {
        public final String ip;
        public final int port;
        public final int generation;

        public View(String ip, int port, int generation) {
            this.ip = ip;
            this.port = port;
            this.generation = generation;
        }
    }

    /** @return {@code current} when the resolved address did not change */
    public static View swap(View current, String resolvedIp, int port) {
        String ip = HostCheck.normalizeAddress(resolvedIp);
        if (ip == null || port < 1 || port > 65535) return current;
        if (current != null && ip.equals(current.ip) && port == current.port) return current;
        int generation = current == null ? 1 : current.generation + 1;
        return new View(ip, port, generation);
    }
}
