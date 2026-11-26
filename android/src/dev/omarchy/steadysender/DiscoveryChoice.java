package dev.omarchy.steadysender;

/**
 * Prefer the source address of the discover reply. The JSON {@code lan}
 * field is only used when that source cannot be sent to.
 */
public final class DiscoveryChoice {
    private DiscoveryChoice() {}

    public static String target(String sourceAddress, String lanField) {
        String source = HostCheck.normalizeAddress(sourceAddress);
        if (source != null && HostCheck.isUsableSource(source)) return source;
        String lan = HostCheck.normalizeAddress(lanField);
        if (lan != null && HostCheck.isIpLiteralOrHostname(lan) && HostCheck.isUsableSource(lan)) {
            return lan;
        }
        return null;
    }
}
