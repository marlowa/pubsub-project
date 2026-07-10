package com.pubsub.fixtestclient.fix;

import java.time.Instant;

/**
 * Wall-clock time source with nanosecond resolution that does not depend on the
 * resolution of {@link Instant#now()}.
 *
 * <p>{@code Instant.now()} only guarantees the resolution the underlying platform
 * clock happens to provide: millisecond on some JDK/OS combinations, microsecond
 * or nanosecond on others. The proprietary gateway rejects any UTCTimestamp whose
 * sub-second field is not genuine nanoseconds, so a client that reads the clock
 * through {@code Instant.now()} produces valid timestamps on one machine and
 * millisecond-padded ones on another. The behaviour must not depend on where the
 * client runs.
 *
 * <p>This clock anchors a wall-clock reading ({@link System#currentTimeMillis()})
 * to a monotonic reading ({@link System#nanoTime()}) once, then derives every
 * later instant by adding the elapsed monotonic nanoseconds to the anchor. Both
 * {@code currentTimeMillis()} and {@code nanoTime()} have identical, well-defined
 * resolution on every JDK, so the result is portable: millisecond-accurate wall
 * time carrying nanosecond-resolution sub-second digits.
 *
 * <p>The wall and monotonic clocks drift apart over time (NTP steps, slew). The
 * anchor is therefore re-taken whenever the derived time diverges from the
 * millisecond clock by more than {@link #MAX_DRIFT_NANOS}, bounding the error.
 */
public final class NanoClock {

    private static final long NANOS_PER_MILLI = 1_000_000L;
    private static final long NANOS_PER_SECOND = 1_000_000_000L;
    private static final long MAX_DRIFT_NANOS = 10L * NANOS_PER_MILLI;

    private long anchorEpochNanos;
    private long anchorNanoTime;

    public NanoClock() {
        anchor();
    }

    private void anchor() {
        anchorEpochNanos = System.currentTimeMillis() * NANOS_PER_MILLI;
        anchorNanoTime = System.nanoTime();
    }

    public synchronized Instant now() {
        long epochNanos = anchorEpochNanos + (System.nanoTime() - anchorNanoTime);
        long wallNanos = System.currentTimeMillis() * NANOS_PER_MILLI;
        if (Math.abs(wallNanos - epochNanos) > MAX_DRIFT_NANOS) {
            anchor();
            epochNanos = anchorEpochNanos;
        }
        return Instant.ofEpochSecond(Math.floorDiv(epochNanos, NANOS_PER_SECOND),
                                     Math.floorMod(epochNanos, NANOS_PER_SECOND));
    }
}
