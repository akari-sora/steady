package dev.omarchy.steadysender;

/**
 * Session-owned wake lock decision. Not thread-safe by itself: the service
 * holds one lock across {@link #tryRenew}, {@link #release}, and the
 * PowerManager call. A permit read before {@link #release} must not acquire.
 */
public final class WakeGate {
    private boolean active;
    private boolean stopped;
    private boolean held;

    public void arm() {
        active = true;
        stopped = false;
        held = true;
    }

    public boolean tryRenew(boolean earlyPermit) {
        if (!earlyPermit || !active || stopped) return false;
        held = true;
        return true;
    }

    public void release() {
        active = false;
        stopped = true;
        held = false;
    }

    public boolean isHeld() {
        return held;
    }
}
