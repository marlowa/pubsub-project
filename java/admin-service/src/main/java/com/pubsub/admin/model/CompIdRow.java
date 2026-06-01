package com.pubsub.admin.model;

import java.time.OffsetDateTime;

/** Full DB row. SCRAM fields (storedKey, serverKey, salt, iterations) are never rendered in templates. */
public record CompIdRow(
        String compId,
        String firmId,
        String storedKey,
        String serverKey,
        String salt,
        int iterations,
        boolean enabled,
        boolean forcePasswordChange,
        int consecutiveFailedLogins,
        boolean locked,
        String lockedReason,
        OffsetDateTime lockedAt,
        OffsetDateTime lastLoginAt,
        OffsetDateTime passwordChangedAt,
        OffsetDateTime createdAt,
        OffsetDateTime updatedAt
) {}
