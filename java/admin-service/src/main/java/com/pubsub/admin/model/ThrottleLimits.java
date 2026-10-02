package com.pubsub.admin.model;

/**
 * A comp id's three gateway throttles: the most new orders, amends and cancels that one of its
 * sessions may send in any one second. Each session of the comp id counts its own commands.
 *
 * <p>Zero means no limit, and is the default. The largest permitted value is
 * {@link #MAX_PERMITTED_PER_SECOND}: the venue takes tens of microseconds to process an order
 * from start to end, so one command every 10 microseconds is the fastest any member could
 * usefully send, and a gateway holds eight bytes for each command a limit allows. The database
 * and the gateways check the same range. See docs/venue/gateway_throttles.md.
 *
 * <p>A value outside the range cannot be constructed, so a mistake is caught where it is typed,
 * not when the member logs on.
 */
public record ThrottleLimits(int maxPlacePerSecond, int maxAmendPerSecond, int maxCancelPerSecond) {

    /** The largest limit that may be configured for any one kind of command. */
    public static final int MAX_PERMITTED_PER_SECOND = 100_000;

    /** No limit on any kind of command, which is what a comp id gets unless told otherwise. */
    public static final ThrottleLimits NONE = new ThrottleLimits(0, 0, 0);

    public ThrottleLimits {
        requireInRange(maxPlacePerSecond, "new orders");
        requireInRange(maxAmendPerSecond, "amends");
        requireInRange(maxCancelPerSecond, "cancels");
    }

    private static void requireInRange(int limit, String kind) {
        if (limit < 0 || limit > MAX_PERMITTED_PER_SECOND) {
            throw new IllegalArgumentException("The limit on " + kind + " per second must be from 0 to " + MAX_PERMITTED_PER_SECOND
                    + " (0 means no limit). Got: " + limit);
        }
    }
}
