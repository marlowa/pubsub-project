package com.pubsub.admin.model;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class ThrottleLimitsTest {

    @Test
    void zeroAndTheLargestPermittedValueAreAccepted() {
        ThrottleLimits limits = new ThrottleLimits(0, 1, ThrottleLimits.MAX_PERMITTED_PER_SECOND);
        assertEquals(0, limits.maxPlacePerSecond());
        assertEquals(1, limits.maxAmendPerSecond());
        assertEquals(100_000, limits.maxCancelPerSecond());
    }

    @Test
    void noneHasNoLimitOnAnyKind() {
        assertEquals(new ThrottleLimits(0, 0, 0), ThrottleLimits.NONE);
    }

    @Test
    void aNegativeLimitIsRefusedNamingTheKind() {
        IllegalArgumentException refusal = assertThrows(IllegalArgumentException.class, () -> new ThrottleLimits(-1, 0, 0));
        assertTrue(refusal.getMessage().contains("new orders"), refusal.getMessage());
    }

    @Test
    void aLimitAboveTheLargestPermittedIsRefusedNamingTheKind() {
        IllegalArgumentException amendRefusal = assertThrows(IllegalArgumentException.class, () -> new ThrottleLimits(0, 100_001, 0));
        assertTrue(amendRefusal.getMessage().contains("amends"), amendRefusal.getMessage());
        IllegalArgumentException cancelRefusal = assertThrows(IllegalArgumentException.class, () -> new ThrottleLimits(0, 0, 100_001));
        assertTrue(cancelRefusal.getMessage().contains("cancels"), cancelRefusal.getMessage());
    }
}
