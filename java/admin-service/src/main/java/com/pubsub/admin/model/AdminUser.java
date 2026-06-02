package com.pubsub.admin.model;

public record AdminUser(
        String username,
        String passwordHash,
        AdminRole role,
        boolean forcePasswordChange) {}
