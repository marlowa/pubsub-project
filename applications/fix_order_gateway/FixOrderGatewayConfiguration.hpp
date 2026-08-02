#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace fix_order_gateway {

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
struct FixOrderGatewayConfiguration {
    /**
     * @brief Which instance of this gateway protocol this process is, numbered from 1.
     *
     * Stamped onto every order envelope beside the protocol id. A protocol may run as
     * several instances for availability, and a session connection id is only unique
     * within one process, so it is the triple (protocol, instance, connection) that
     * identifies a session venue-wide and lets the sequencer route the execution report
     * back to the process the order arrived on.
     *
     * Must match a [[gateway]] entry in the sequencer's configuration; the sequencer
     * warns if it sees a pair it has no endpoint for, because it cannot deliver reports
     * to a gateway it was never told about.
     */
    int16_t instance_id{1};

    // Inbound FIX listener

    /** @brief Host address on which the gateway listens for FIX client connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief Plain TCP port on which the gateway listens for FIX client connections. */
    uint16_t listen_port{9879};

    /** @brief TLS port for FIX client connections (only used when fix_tls_enabled=true). */
    uint16_t tls_listen_port{9880};

    /** @brief Size in bytes of the per-connection raw receive buffer. */
    int64_t raw_buffer_capacity{65536};

    // Sequencer outbound connection
    //
    // The gateway maintains an outbound TCP PDU connection to the primary
    // sequencer instance. The fan-out path to a follower is part of the
    // leader-follower protocol and not yet implemented; until that lands,
    // only the primary sequencer is configured here.

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

    // Matching engine inbound ER connection
    //
    // The matching engine sends ExecutionReport PDUs back to the gateway
    // over a direct TCP PDU connection. This is a stub for the future
    // pub/sub fanout path.

    /** @brief Host address on which the gateway listens for ER PDUs from the ME. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for ER PDUs from the ME. */
    uint16_t er_listen_port{7010};

    // Authentication service outbound connection
    //
    // The gateway connects to the authentication service as a plain TCP PDU
    // client. Each FIX Logon triggers a SCRAM-SHA-256 exchange; the gateway
    // only completes the FIX session once the exchange returns Granted and the
    // ServerSignature is verified.

    /** @brief Host address of the primary authentication service. */
    std::string authentication_service_host{"127.0.0.1"};

    /** @brief TCP port of the primary authentication service. */
    uint16_t authentication_service_port{7070};

    /** @brief Host address of the secondary authentication service. Only used when ha_enabled=true. */
    std::string authentication_service_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary authentication service. Only used when ha_enabled=true. */
    uint16_t authentication_service_secondary_port{7071};

    // FIX listener TLS
    //
    // When fix_tls_enabled is true the FIX listener uses TLS (ProtocolType::TlsRawBytes).
    // cert_path and key_path are PEM files, resolved relative to the process
    // working directory (the component's etc/<name>/ directory in the install
    // tree).  Client certificate verification is not required: FIX clients
    // authenticate via SCRAM-SHA-256 in the application layer.

    /** @brief Enable TLS on the inbound FIX listener. */
    bool fix_tls_enabled{false};

    /** @brief Path to the PEM server certificate for the FIX TLS listener. */
    std::string fix_tls_cert_path;

    /** @brief Path to the PEM private key for the FIX TLS listener. */
    std::string fix_tls_key_path;

    // FIX session identity

    /** @brief SenderCompID used in all outbound FIX messages. */
    std::string sender_comp_id{"GATEWAY"};

    /** @brief Default TargetCompID used before a Logon has been received. */
    std::string default_target_comp_id{"CLIENT"};

    // Timeouts

    /** @brief Maximum time allowed for a newly connected FIX client to send a Logon. */
    std::chrono::seconds logon_timeout{30};

    /** @brief Maximum time allowed for a SCRAM-SHA-256 exchange to complete after Logon is received. */
    std::chrono::seconds scram_auth_timeout{10};

    // Cancel-on-disconnect

    /**
     * @brief Whether a lost session's resting orders are cancelled at all.
     *
     * Defaults on: an unmanaged book sitting behind a session nobody is watching is the
     * worse of the two failures. Turning it off leaves every order resting and makes the
     * member wholly responsible for its own book across a disconnect.
     *
     * Venue-wide here. Per comp id belongs in provisioning -- the admin service and the
     * database already own comp ids -- and is deliberately a later step.
     */
    bool cancel_on_disconnect_enabled{true};

    /**
     * @brief How long a dropped session's orders are left resting before being cancelled.
     *
     * **This is what makes gateway failover coherent, not a refinement of it.** Without a
     * delay, losing a gateway process flattens every book on it the instant the sockets
     * close -- the high-availability mechanism producing precisely the outcome high
     * availability exists to prevent. Set comfortably longer than a client takes to notice
     * a dead gateway and reconnect to its backup, and a gateway failure becomes a
     * reconnect rather than a mass cancellation.
     *
     * If the same comp id logs on again inside the window its orders are reclaimed and
     * nothing is cancelled. Zero restores the old immediate behaviour.
     *
     * A clean FIX Logout ignores this and cancels at once: a member that logs out has said
     * what it wants, whereas a socket that vanished has not.
     */
    std::chrono::seconds cancel_on_disconnect_grace_period{30};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the applog */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;

    // Reactor

    /** @brief Enable CPU core pinning for registered application threads.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_enabled;

    /** @brief Exclude CPU 0 from pinning candidates (for machines without isolated cores).
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_reserve_cpu0;

    /** @brief Path to the shared CPU registry file, and to the flock file that serialises
     *  access to it. Both live under the deployment's run directory so that two
     *  installations on one machine cannot contend for a single registry.
     *  Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_registry_shm_path;
    std::string cpu_registry_lock_file;

    /** The machine-wide CPU layout file written by deploy.py, and this
     *  component's key within it (e.g. "sequencer_secondary" -- the instance,
     *  not the binary, since a primary and its secondary are placed separately).
     *  Cores are allocated at deploy time, not negotiated at run time.
     *  Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_layout_file;
    std::string cpu_layout_component;

    /** @brief How long to wait between "still disconnected" log warnings during outbound retry. */
    std::chrono::milliseconds connect_retry_warning_interval;

    // Event queue pool  (ApplicationThread inbound EventMessage queue)

    /** @brief Number of objects in each fixed-size memory pool slab.
     *  Increase if event-queue pool-exhaustion warnings appear in the log. */
    int32_t event_queue_pool_objects_per_slab{64};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // Command queue pool  (Reactor ReactorControlCommand outbound queue)

    /** @brief Number of objects in each fixed-size memory pool slab.
     *  Increase if command-queue pool-exhaustion warnings appear in the log. */
    int32_t command_queue_pool_objects_per_slab{64};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};

    // FIX capture

    /** @brief Whether to capture all inbound and outbound FIX wire bytes to a file.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool fix_capture_enabled;

    /** @brief Path to the binary FIX capture file.
     *  Required when fix_capture_enabled=true; ignored otherwise.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    std::string fix_capture_file;

    /** @brief Byte capacity of the lock-free ring buffer used by the FIX capture
     *  writer thread.  The ring is pre-allocated once at startup; records are
     *  packed into it with no per-record heap allocation.  If the writer falls
     *  behind and the ring fills, records are dropped with a Warning.  64 MB
     *  (67108864) is ample for most workloads.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    int64_t fix_capture_ring_bytes{67108864};

    // Wall clock

    /** @brief Clock used to generate SendingTime (tag 52) in all outbound FIX messages.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to produce deterministic timestamps in tests. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    // FIX field-length limits

    // Runtime field-length limits for inbound FIX strings (symbol, qty). Must be <= the
    // compile-time hard ceilings in FixSession.hpp; over-length values get a FIX BusinessReject.
    // ClOrdID is NOT here: it is the shared compile-time fix_order_limits::max_cl_ord_id_length.
    int32_t max_symbol_length{32};
    int32_t max_order_qty_length{24};

    // Open-order pool

    // Pool for open-order string storage.
    int32_t open_order_pool_objects_per_pool{4096};
    int32_t open_order_pool_initial_pools{1};

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/design/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;
};

} // namespaces
