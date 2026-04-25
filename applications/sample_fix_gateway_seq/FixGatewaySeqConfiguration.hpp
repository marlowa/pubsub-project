#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

namespace sample_fix_gateway_seq {

/**
 * @brief Configuration for the sequencer-backed FIX gateway application.
 *
 * Extends the simple gateway configuration with two sequencer endpoints
 * (primary and secondary) and one inbound ER endpoint from the matching
 * engine. The gateway connects outbound to both sequencer instances so that
 * the follower stays in sync and failover is gap-free.
 *
 * All fields have sensible defaults suitable for local development.
 */
struct FixGatewaySeqConfiguration {
    // ----------------------------------------------------------------
    // Inbound FIX listener
    // ----------------------------------------------------------------

    /** @brief Host address on which the gateway listens for FIX client connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for FIX client connections. */
    uint16_t listen_port{9879};

    /** @brief Size in bytes of the per-connection raw receive buffer. */
    int64_t raw_buffer_capacity{65536};

    // ----------------------------------------------------------------
    // Sequencer outbound connections
    //
    // The gateway maintains outbound TCP PDU connections to both the primary
    // and secondary sequencer instances. Every order PDU is sent to both so
    // the follower stays in sync with the leader at all times.
    // ----------------------------------------------------------------

    /** @brief Host address of the primary sequencer. */
    std::string sequencer_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary sequencer. */
    uint16_t sequencer_primary_port{7001};

    /** @brief Host address of the secondary sequencer. */
    std::string sequencer_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary sequencer. */
    uint16_t sequencer_secondary_port{7002};

    // ----------------------------------------------------------------
    // Matching engine inbound ER connection
    //
    // The matching engine sends ExecutionReport PDUs back to the gateway
    // over a direct TCP PDU connection. This is a stub for the future
    // pub/sub fanout path.
    // ----------------------------------------------------------------

    /** @brief Host address on which the gateway listens for ER PDUs from the ME. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for ER PDUs from the ME. */
    uint16_t er_listen_port{7010};

    // ----------------------------------------------------------------
    // FIX session identity
    // ----------------------------------------------------------------

    /** @brief SenderCompID used in all outbound FIX messages. */
    std::string sender_comp_id{"GATEWAY"};

    /** @brief Default TargetCompID used before a Logon has been received. */
    std::string default_target_comp_id{"CLIENT"};

    // ----------------------------------------------------------------
    // Timeouts
    // ----------------------------------------------------------------

    /** @brief Maximum time allowed for a newly connected FIX client to send a Logon. */
    std::chrono::seconds logon_timeout{30};
};

} // namespace sample_fix_gateway_seq
