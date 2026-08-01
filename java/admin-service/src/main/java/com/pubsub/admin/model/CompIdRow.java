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
        OffsetDateTime updatedAt,
        /**
         * Cancel-on-disconnect for this comp id: what a gateway does with the session's
         * resting orders when its connection goes away.
         *
         * cancelOnDisconnectGracePeriodSeconds is boxed rather than an int because null is
         * a meaningful value here, distinct from zero: it means this member expressed no
         * preference and the gateway's own configured default applies. Zero means cancel
         * immediately. An operator raising the venue-wide window must not have to revisit
         * every member, so the distinction has to survive all the way to the gateway.
         */
        boolean cancelOnDisconnectEnabled,
        Integer cancelOnDisconnectGracePeriodSeconds
) {}
