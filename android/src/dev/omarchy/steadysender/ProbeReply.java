package dev.omarchy.steadysender;

/**
 * True when the whole datagram is one JSON object whose {@code steady}
 * member is the boolean {@code true}. A substring, a string value, or
 * trailing junk does not count.
 */
public final class ProbeReply {
    private ProbeReply() {}

    public static boolean isSteady(String body) {
        if (body == null) return false;
        int n = body.length();
        int i = skipWs(body, 0);
        if (i >= n || body.charAt(i) != '{') return false;
        i++;
        boolean saw = false;
        boolean first = true;
        while (true) {
            i = skipWs(body, i);
            if (i < n && body.charAt(i) == '}') {
                i = skipWs(body, i + 1);
                return saw && i == n;
            }
            if (!first) {
                if (i >= n || body.charAt(i) != ',') return false;
                i = skipWs(body, i + 1);
            }
            first = false;
            if (i >= n || body.charAt(i) != '"') return false;
            int keyEnd = scanStringEnd(body, i + 1);
            if (keyEnd < 0) return false;
            boolean steadyKey = "steady".equals(body.substring(i + 1, keyEnd));
            i = skipWs(body, keyEnd + 1);
            if (i >= n || body.charAt(i) != ':') return false;
            i = skipWs(body, i + 1);
            if (steadyKey) {
                if (region(body, i, "true")) {
                    saw = true;
                    i += 4;
                } else {
                    return false;
                }
            } else {
                i = skipValue(body, i);
                if (i < 0) return false;
            }
        }
    }

    private static boolean region(String s, int i, String token) {
        return i >= 0 && i + token.length() <= s.length() && s.regionMatches(i, token, 0, token.length());
    }

    private static int skipWs(String s, int i) {
        while (i < s.length()) {
            char c = s.charAt(i);
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            i++;
        }
        return i;
    }

    private static int scanStringEnd(String s, int i) {
        for (int j = i; j < s.length(); j++) {
            char c = s.charAt(j);
            if (c == '\\') {
                j++;
                continue;
            }
            if (c == '"') return j;
        }
        return -1;
    }

    private static int skipValue(String s, int i) {
        if (i >= s.length()) return -1;
        char c = s.charAt(i);
        if (c == '"') {
            int end = scanStringEnd(s, i + 1);
            return end < 0 ? -1 : end + 1;
        }
        if (c == '{') return skipContainer(s, i, '{', '}');
        if (c == '[') return skipContainer(s, i, '[', ']');
        if (region(s, i, "true")) return i + 4;
        if (region(s, i, "false")) return i + 5;
        if (region(s, i, "null")) return i + 4;
        if (c == '-' || (c >= '0' && c <= '9')) {
            int j = i + 1;
            while (j < s.length()) {
                char d = s.charAt(j);
                if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
                    j++;
                } else {
                    break;
                }
            }
            return j;
        }
        return -1;
    }

    private static int skipContainer(String s, int i, char open, char close) {
        int depth = 0;
        for (int j = i; j < s.length(); j++) {
            char c = s.charAt(j);
            if (c == '"') {
                int end = scanStringEnd(s, j + 1);
                if (end < 0) return -1;
                j = end;
                continue;
            }
            if (c == open) depth++;
            else if (c == close) {
                depth--;
                if (depth == 0) return j + 1;
            }
        }
        return -1;
    }
}
