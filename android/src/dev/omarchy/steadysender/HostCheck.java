package dev.omarchy.steadysender;

/** IP literal and plain hostname checks. No network calls. */
public final class HostCheck {
    private HostCheck() {}

    public static Integer portOrNull(String raw) {
        if (raw == null) return null;
        String t = raw.trim();
        if (t.isEmpty() || t.length() > 5) return null;
        int p = 0;
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            if (c < '0' || c > '9') return null;
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) return null;
        return p;
    }

    public static boolean isIpLiteralOrHostname(String raw) {
        if (raw == null) return false;
        String s = raw.trim();
        if (s.isEmpty() || s.length() > 253) return false;
        if (looksLikeIpv4(s)) return isIpv4(s);
        if (s.indexOf(':') >= 0) return isIpv6(s);
        return isHostname(s);
    }

    /**
     * A packet source we can send to. Wildcard, broadcast and loopback are
     * not usable, so discovery falls back to the reply's lan field.
     */
    public static boolean isUsableSource(String raw) {
        if (!isIpLiteralOrHostname(raw)) return false;
        String s = raw.trim();
        if ("0.0.0.0".equals(s) || "255.255.255.255".equals(s) || "::".equals(s)) {
            return false;
        }
        if ("localhost".equalsIgnoreCase(s)) return false;
        if (s.startsWith("127.")) return false;
        if ("::1".equals(s)) return false;
        if (isIpv4(s)) {
            int first = Integer.parseInt(s.substring(0, s.indexOf('.')));
            if (first >= 224) return false;
        }
        return true;
    }

    /** Trim, and drop an IPv6 zone id ({@code %wlan0}) if present. */
    public static String normalizeAddress(String raw) {
        if (raw == null) return null;
        String s = raw.trim();
        if (s.isEmpty()) return null;
        int zone = s.indexOf('%');
        if (zone > 0) s = s.substring(0, zone);
        return s;
    }

    private static boolean looksLikeIpv4(String s) {
        int dots = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '.') dots++;
            else if (c < '0' || c > '9') return false;
        }
        return dots == 3;
    }

    static boolean isIpv4(String s) {
        int start = 0;
        int octets = 0;
        for (int i = 0; i <= s.length(); i++) {
            if (i == s.length() || s.charAt(i) == '.') {
                int len = i - start;
                if (len < 1 || len > 3) return false;
                int v = 0;
                for (int k = start; k < i; k++) {
                    char c = s.charAt(k);
                    if (c < '0' || c > '9') return false;
                    v = v * 10 + (c - '0');
                }
                if (v > 255) return false;
                octets++;
                start = i + 1;
            }
        }
        return octets == 4;
    }

    static boolean isIpv6(String s) {
        if (s.indexOf('%') >= 0) return false;
        int dc = s.indexOf("::");
        if (dc >= 0) {
            if (s.indexOf("::", dc + 2) >= 0) return false;
            String left = s.substring(0, dc);
            String right = s.substring(dc + 2);
            if (!validGroups(left) || !validGroups(right)) return false;
            int groups = countGroups(left) + countGroups(right);
            return groups <= 7;
        }
        return countGroups(s) == 8 && validGroups(s);
    }

    private static int countGroups(String s) {
        if (s.isEmpty()) return 0;
        int n = 1;
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) == ':') n++;
        return n;
    }

    private static boolean validGroups(String s) {
        if (s.isEmpty()) return true;
        int len = 0;
        for (int i = 0; i <= s.length(); i++) {
            if (i == s.length() || s.charAt(i) == ':') {
                if (len < 1 || len > 4) return false;
                len = 0;
            } else {
                char c = s.charAt(i);
                boolean hex = (c >= '0' && c <= '9')
                    || (c >= 'a' && c <= 'f')
                    || (c >= 'A' && c <= 'F');
                if (!hex) return false;
                len++;
            }
        }
        return true;
    }

    static boolean isHostname(String s) {
        if (s.isEmpty() || s.length() > 253) return false;
        if (s.charAt(0) == '.' || s.charAt(s.length() - 1) == '.') return false;
        int label = 0;
        for (int i = 0; i <= s.length(); i++) {
            if (i == s.length() || s.charAt(i) == '.') {
                if (label < 1 || label > 63) return false;
                label = 0;
                continue;
            }
            char c = s.charAt(i);
            if (label == 0 && i > 0 && s.charAt(i - 1) == '.') {
                // start of a label
            }
            boolean ok = (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9')
                || c == '-';
            if (!ok) return false;
            if (c == '-' && (label == 0 || i + 1 == s.length() || s.charAt(i + 1) == '.')) {
                return false;
            }
            label++;
        }
        return true;
    }
}
