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
        Integer cancelOnDisconnectGracePeriodSeconds,
        /**
         * The gateway instances this comp id's session may log on to: the one it is
         * expected to use, and the one it falls back to when that is unreachable. A
         * gateway refuses a logon from a member pinned elsewhere.
         *
         * Both boxed because null means "not pinned" -- this member may log on to any
         * instance -- which is a different statement from any instance number, and
         * instance 0 does not exist. A primary with a null backup pins the member to
         * exactly one instance.
         *
         * These name an instance, not a protocol: instance 1 of the FIX gateway and
         * instance 1 of the binary gateway are separate processes holding the same
         * position in their own protocol, and a member's pinning applies to whichever
         * it speaks.
         */
        Integer primaryGatewayInstance,
        Integer backupGatewayInstance
) {}
