package com.pubsub.admin.model;

import java.time.OffsetDateTime;

public record GatewayPermissionRow(
        String compId,
        String gatewayType,
        boolean enabled,
        OffsetDateTime createdAt
) {}
