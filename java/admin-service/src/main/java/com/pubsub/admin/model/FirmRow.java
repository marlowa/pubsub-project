package com.pubsub.admin.model;

import java.time.OffsetDateTime;

public record FirmRow(
        String firmId,
        String name,
        boolean enabled,
        OffsetDateTime createdAt,
        OffsetDateTime updatedAt
) {}
