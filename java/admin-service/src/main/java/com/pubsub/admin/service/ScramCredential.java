package com.pubsub.admin.service;

/** Hex-encoded SCRAM-SHA-256 credential material derived from a plaintext password. */
public record ScramCredential(
        String storedKey,
        String serverKey,
        String salt,
        int iterations
) {}
