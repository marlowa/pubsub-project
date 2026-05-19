#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>

namespace sample_fix_gateway_seq {

/**
 * @brief Configuration for the sequencer-backed FIX gateway application.
 *
 * Extends the simple gateway configuration with one sequencer endpoint
 * (the primary) and one inbound ER endpoint from the matching engine.
 *
 * Until the leader-follower protocol lands, only the primary sequencer is
 * connected. Reintroducing a follower endpoint is part of that work.
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
    // Sequencer outbound connection
    //
    // The gateway maintains an outbound TCP PDU connection to the primary
    // sequencer instance. The fan-out path to a follower is part of the
    // leader-follower protocol and not yet implemented; until that lands,
    // only the primary sequencer is configured here.
    // ----------------------------------------------------------------

    /**
     * @brief Enable HA dual-publish to a secondary sequencer. When false (default),
     * only the primary sequencer is connected. When true, the gateway also connects
     * to and dual-publishes every order PDU to the secondary sequencer.
     */
    bool ha_enabled{false};

    /** @brief Host address of the primary sequencer. */
    std::string sequencer_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary sequencer. */
    uint16_t sequencer_primary_port{7001};

    /** @brief Host address of the secondary (follower) sequencer. Only used when ha_enabled=true. */
    std::string sequencer_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary (follower) sequencer. Only used when ha_enabled=true. */
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

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the applog */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;
};

} // namespace sample_fix_gateway_seq
