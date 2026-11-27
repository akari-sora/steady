package dev.omarchy.steadysender;

/**
 * Parsed {@code steady://connect} target. A non-null {@link #error} means
 * the link must not change the saved host or port. Callers show a
 * confirmation dialog before applying a valid result.
 */
public final class DeepLinkRequest {
    public final String host;
    public final int port;
    public final String lan;
    public final String ts;
    public final String error;

    private DeepLinkRequest(String host, int port, String lan, String ts, String error) {
        this.host = host;
        this.port = port;
        this.lan = lan;
        this.ts = ts;
        this.error = error;
    }

    public boolean ok() {
        return error == null;
    }

    /**
     * Every field {@link #applyAllowed} would write, as {@code name: before → after}.
     * The dialog must show this text. A host-only sentence does not disclose lan or ts.
     */
    public String confirmMessage(String curHost, String curPort, String curLan, String curTs) {
        return confirmMessage(curHost, curPort, curLan, curTs, "(空)");
    }

    /**
     * Same message, with a caller-supplied (localised) label for empty fields.
     * The activity passes {@code getString(R.string.empty_label)} here.
     */
    public String confirmMessage(String curHost, String curPort, String curLan, String curTs,
                                 String emptyLabel) {
        if (!ok()) return "";
        StringBuilder sb = new StringBuilder();
        if (host != null) line(sb, "host", curHost, host, emptyLabel);
        if (port > 0) line(sb, "port", curPort, String.valueOf(port), emptyLabel);
        if (lan != null) line(sb, "lan", curLan, lan, emptyLabel);
        if (ts != null) line(sb, "ts", curTs, ts, emptyLabel);
        return sb.toString();
    }

    /** True when {@code message} names every field this link would change. */
    public boolean disclosed(String message) {
        if (!ok() || message == null) return false;
        if (host == null && port <= 0 && lan == null && ts == null) return false;
        if (host != null && (!message.contains("host: ") || !message.contains("→ " + host))) return false;
        if (port > 0 && (!message.contains("port: ") || !message.contains("→ " + port))) return false;
        if (lan != null && (!message.contains("lan: ") || !message.contains("→ " + lan))) return false;
        if (ts != null && (!message.contains("ts: ") || !message.contains("→ " + ts))) return false;
        return true;
    }

    public boolean applyAllowed(String curHost, String curPort, String curLan, String curTs) {
        return disclosed(confirmMessage(curHost, curPort, curLan, curTs));
    }

    private static void line(StringBuilder sb, String name, String before, String after,
                             String emptyLabel) {
        String from = before == null ? "" : before.trim();
        if (from.isEmpty()) from = emptyLabel;
        sb.append(name).append(": ").append(from).append(" → ").append(after).append('\n');
    }

    /**
     * Hierarchical {@code steady://connect} link. Same as
     * {@link #parse(boolean, String, String, String, String, String, String, boolean)}
     * with a readable query.
     */
    public static DeepLinkRequest parse(String scheme, String uriHost,
                                         String host, String port, String lan, String ts) {
        return parse(true, scheme, uriHost, host, port, lan, ts, false);
    }

    /**
     * @param hierarchical false for an opaque URI such as {@code steady:connect?host=…}
     * @param queryUnreadable true when reading the query threw or was refused
     * @param host query value, or null when the parameter is absent
     * @param port query value, or null when the parameter is absent
     * @return null when this intent is not a connect link. A steady link that
     *         is opaque or whose query cannot be read is rejected and carries
     *         no host to apply.
     */
    public static DeepLinkRequest parse(boolean hierarchical, String scheme, String uriHost,
                                         String host, String port, String lan, String ts,
                                         boolean queryUnreadable) {
        if (!hierarchical || queryUnreadable) {
            if (!"steady".equals(scheme)) return null;
            return reject("bad uri");
        }
        if (!"steady".equals(scheme) || uriHost == null || !"connect".equalsIgnoreCase(uriHost)) {
            return null;
        }
        if (host == null && port == null && lan == null && ts == null) {
            return null;
        }

        String hostOut = null;
        if (host != null) {
            hostOut = host.trim();
            if (!HostCheck.isUsableSource(hostOut)) {
                return reject("bad host");
            }
        }
        int portOut = -1;
        if (port != null) {
            Integer p = HostCheck.portOrNull(port);
            if (p == null) return reject("bad port");
            portOut = p;
        }
        String lanOut = optionalHost(lan);
        if (lan != null && lanOut == null) return reject("bad host");
        String tsOut = optionalHost(ts);
        if (ts != null && tsOut == null) return reject("bad host");
        return new DeepLinkRequest(hostOut, portOut, lanOut, tsOut, null);
    }

    private static DeepLinkRequest reject(String error) {
        return new DeepLinkRequest(null, -1, null, null, error);
    }

    private static String optionalHost(String raw) {
        if (raw == null) return null;
        String s = raw.trim();
        if (!HostCheck.isUsableSource(s)) return null;
        return s;
    }
}
