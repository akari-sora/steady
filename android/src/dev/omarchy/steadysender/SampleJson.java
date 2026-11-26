package dev.omarchy.steadysender;

/**
 * Allocation-free ASCII JSON for one IMU packet. Keys stay in the historical
 * order; {@code idle} is added only on stationary keepalives and sample
 * values are never forced to zero.
 */
public final class SampleJson {
    private SampleJson() {}

    public static int encode(byte[] dest, StringBuilder sb,
                              double ax, double ay, double az, double gz,
                              boolean grav, boolean idle) {
        sb.setLength(0);
        sb.append("{\"ax\":");
        appendFixed3(sb, ax);
        sb.append(",\"ay\":");
        appendFixed3(sb, ay);
        sb.append(",\"az\":");
        appendFixed3(sb, az);
        sb.append(",\"gz\":");
        appendFixed3(sb, gz);
        sb.append(",\"grav\":");
        sb.append(grav ? "true" : "false");
        if (idle) sb.append(",\"idle\":true");
        sb.append(",\"src\":\"Steady Android\"}");

        int n = sb.length();
        if (n > dest.length) return 0;
        for (int i = 0; i < n; i++) {
            dest[i] = (byte) sb.charAt(i);
        }
        return n;
    }

    static void appendFixed3(StringBuilder out, double v) {
        if (!Double.isFinite(v)) {
            out.append('0');
            return;
        }
        if (v > 1000.0) v = 1000.0;
        else if (v < -1000.0) v = -1000.0;
        long scaled = Math.round(v * 1000.0);
        if (scaled < 0) {
            out.append('-');
            scaled = -scaled;
        }
        long whole = scaled / 1000L;
        long frac = scaled % 1000L;
        out.append(whole).append('.');
        if (frac < 100L) out.append('0');
        if (frac < 10L) out.append('0');
        out.append(frac);
    }
}
