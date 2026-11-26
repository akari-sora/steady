package dev.omarchy.steadysender;

/**
 * A discovery reply is an offer until the user confirms it. Nothing is
 * stored on the offer alone.
 */
public final class DiscoveryCommit {
    public final String host;
    public final String ts;
    private final boolean confirmed;

    private DiscoveryCommit(String host, String ts, boolean confirmed) {
        this.host = host;
        this.ts = ts;
        this.confirmed = confirmed;
    }

    public boolean persist() {
        return confirmed;
    }

    /** @return null when the host cannot be sent to */
    public static DiscoveryCommit offer(String host, String ts) {
        if (host == null) return null;
        String h = host.trim();
        if (!HostCheck.isUsableSource(h)) return null;
        String tsOut = null;
        if (ts != null) {
            String t = ts.trim();
            if (HostCheck.isUsableSource(t)) tsOut = t;
        }
        return new DiscoveryCommit(h, tsOut, false);
    }

    public DiscoveryCommit confirm() {
        return new DiscoveryCommit(host, ts, true);
    }
}
