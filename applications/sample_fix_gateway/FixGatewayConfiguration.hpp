#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <string>

namespace sample_fix_gateway {

/**
 * @brief Configuration for the sample FIX 5.0SP2 gateway application.
 *
 * All fields have sensible defaults suitable for local development and testing.
 * In production, a TOML loader would populate this struct from a configuration
 * file before passing it to SampleFixGateway.
 *
 * This struct is intentionally independent of the framework's ReactorConfiguration
 * and HAConfiguration -- it contains only application-level settings.
 */
struct FixGatewayConfiguration {
    // ----------------------------------------------------------------
    // Network
    // ----------------------------------------------------------------

    /**
     * @brief The host address on which the gateway listens for inbound FIX
     *        connections.
     */
    std::string listen_host{"127.0.0.1"};

    /**
     * @brief The TCP port on which the gateway listens for inbound FIX
     *        connections.
     */
    uint16_t listen_port{9878};

    /**
     * @brief Size in bytes of the per-connection raw receive buffer.
     *
     * Must be large enough to hold the largest burst of FIX messages that
     * could arrive between successive CommitRawBytes calls. The MirroredBuffer
     * will disconnect a client that overflows this buffer.
     */
    int64_t raw_buffer_capacity{65536};

    // ----------------------------------------------------------------
    // FIX session identity
    // ----------------------------------------------------------------

    /**
     * @brief SenderCompID used in all outbound FIX messages.
     *
     * Identifies this gateway to connecting clients. Must match the
     * TargetCompID configured in the client's FIX session settings.
     */
    std::string sender_comp_id{"GATEWAY"};

    /**
     * @brief Default TargetCompID used in outbound FIX messages before
     *        a Logon has been received.
     *
     * Once a client sends a Logon, the gateway uses the client's
     * SenderCompID as the TargetCompID for all subsequent replies to
     * that session.
     */
    std::string default_target_comp_id{"CLIENT"};

    // ----------------------------------------------------------------
    // Session timeouts
    // ----------------------------------------------------------------

    /**
     * @brief Maximum time allowed for a newly connected client to send a
     *        valid FIX Logon message.
     *
     * If no Logon is received within this interval the gateway tears down
     * the connection. This protects against clients that connect but never
     * complete the FIX session handshake (e.g. port scanners, telnet probes).
     *
     * Default: 30 seconds.
     */
    std::chrono::seconds logon_timeout{30};
};

} // namespace sample_fix_gateway
