#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace order_gateway {

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
struct OrderGatewayConfiguration {
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
    // Authentication service outbound connection
    //
    // The gateway connects to the authentication service as a plain TCP PDU
    // client. Each FIX Logon triggers a SCRAM-SHA-256 exchange; the gateway
    // only completes the FIX session once the exchange returns Granted and the
    // ServerSignature is verified.
    // ----------------------------------------------------------------

    /** @brief Host address of the primary authentication service. */
    std::string authentication_service_host{"127.0.0.1"};

    /** @brief TCP port of the primary authentication service. */
    uint16_t authentication_service_port{7070};

    /** @brief Host address of the secondary authentication service. Only used when ha_enabled=true. */
    std::string authentication_service_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary authentication service. Only used when ha_enabled=true. */
    uint16_t authentication_service_secondary_port{7071};

    /** @brief SCRAM-SHA-256 password the gateway sends on behalf of connecting FIX clients. */
    std::string scram_password{"stubpassword"};

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

    /** @brief Maximum time allowed for a SCRAM-SHA-256 exchange to complete after Logon is received. */
    std::chrono::seconds scram_auth_timeout{10};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the applog */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;

    // ----------------------------------------------------------------
    // Reactor
    // ----------------------------------------------------------------

    /** @brief Enable CPU core pinning for registered application threads.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_enabled;

    /** @brief Exclude CPU 0 from pinning candidates (for machines without isolated cores).
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_reserve_cpu0;

    /** @brief Path to the flock file used to serialise cross-process CPU registry access.
     *  Prefer /dev/shm/ so the file is cleared on reboot.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    std::string cpu_registry_lock_file;

    // ----------------------------------------------------------------
    // Event queue pool  (ApplicationThread inbound EventMessage queue)
    // ----------------------------------------------------------------

    /** @brief Number of objects in each fixed-size memory pool slab.
     *  Increase if event-queue pool-exhaustion warnings appear in the log. */
    int32_t event_queue_pool_objects_per_slab{64};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // ----------------------------------------------------------------
    // Command queue pool  (Reactor ReactorControlCommand outbound queue)
    // ----------------------------------------------------------------

    /** @brief Number of objects in each fixed-size memory pool slab.
     *  Increase if command-queue pool-exhaustion warnings appear in the log. */
    int32_t command_queue_pool_objects_per_slab{64};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};

    // ----------------------------------------------------------------
    // FIX capture
    // ----------------------------------------------------------------

    /** @brief Whether to capture all inbound and outbound FIX wire bytes to a file.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool fix_capture_enabled;

    /** @brief Path to the binary FIX capture file.
     *  Required when fix_capture_enabled=true; ignored otherwise.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    std::string fix_capture_file;

    /** @brief Maximum number of records the capture queue may hold before
     *  new records are dropped with a warning.  Size for expected burst depth:
     *  at 10,000 messages/second and a writer that flushes at least once per
     *  millisecond, 65536 is ample for development; production high-throughput
     *  deployments should increase this value.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    int32_t fix_capture_queue_depth{65536};

    // ----------------------------------------------------------------
    // Wall clock
    // ----------------------------------------------------------------

    /** @brief Clock used to generate SendingTime (tag 52) in all outbound FIX messages.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to produce deterministic timestamps in tests. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};
};

} // namespace order_gateway
