package com.pubsub.fixtestclient.fix;

import org.junit.jupiter.api.Test;

import java.time.Instant;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

class NanoClockTest {

    @Test
    void now_hasSubMillisecondResolution() {
        NanoClock clock = new NanoClock();

        boolean sawSubMillisecondDigits = false;
        for (int i = 0; i < 5000 && !sawSubMillisecondDigits; i++) {
            if (clock.now().getNano() % 1_000_000L != 0L) {
                sawSubMillisecondDigits = true;
            }
        }

        assertTrue(sawSubMillisecondDigits,
                "NanoClock must expose finer-than-millisecond resolution regardless of the "
                        + "platform Instant clock, so proprietary timestamps are never millisecond-padded");
    }

    @Test
    void now_doesNotGoBackwards() {
        NanoClock clock = new NanoClock();

        Instant previous = clock.now();
        for (int i = 0; i < 1000; i++) {
            Instant current = clock.now();
            assertFalse(current.isBefore(previous), "NanoClock must be monotonic non-decreasing");
            previous = current;
        }
    }

    @Test
    void now_isCloseToWallClock() {
        NanoClock clock = new NanoClock();

        long wallMillis = System.currentTimeMillis();
        long clockMillis = clock.now().toEpochMilli();

        assertTrue(Math.abs(clockMillis - wallMillis) < 1000L,
                "NanoClock must track wall-clock time to within a second");
    }
}
