#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>   // For uint16_t
#include <string>    // For std::string

namespace pubsub_itc_fw {

/**
 * @brief Configuration value type representing a network endpoint.
 *
 * This struct holds the host and port information needed to establish
 * a TCP connection or bind a listening socket. The host may be an IPv4
 * address, an IPv6 address, or a DNS hostname. Resolution is performed
 * later by InetAddress::create().
 *
 * This type is intentionally lightweight and free of OS-specific details.
 * It is suitable for use in configuration files and in ReactorConfiguration.
 */
struct NetworkEndpointConfig {
    /**
     * @brief The host component of the endpoint.
     *
     * This may be:
     *   - an IPv4 dotted-quad string (e.g. "192.168.1.10")
     *   - an IPv6 string (e.g. "::1" or "2001:db8::1")
     *   - a DNS hostname (e.g. "primary.example.com")
     */
    std::string host;

    /**
     * @brief The TCP port number for the endpoint.
     *
     * A value of 0 indicates "not configured" and should be treated as invalid
     * until explicitly set by the application configuration.
     */
    uint16_t port{0};
};

} // namespace pubsub_itc_fw
