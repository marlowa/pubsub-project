#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace matching_engine {

/**
 * @brief Configuration for the matching engine application.
 *
 * The matching engine accepts sequenced order PDUs from the sequencer,
 * matches them, and sends ExecutionReport PDUs back to the sequencer's
 * ER inbound listener. The sequencer then forwards ERs to the gateway.
 * All traffic flows through the sequencer in both directions.
 */
struct MatchingEngineConfiguration {
    // Inbound -- sequenced order PDUs from the sequencer

    /** @brief Host address on which the ME listens for PDUs from the sequencer. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the ME listens for PDUs from the sequencer. */
    uint16_t listen_port{7020};

    // Outbound -- ExecutionReport PDUs back to the sequencer
    //
    // The ME connects outbound to the sequencer's ER inbound listener.
    // The sequencer then forwards ERs to the appropriate gateway.

    /** @brief Host address of the primary sequencer's ER inbound listener. */
    std::string sequencer_er_host{"127.0.0.1"};

    /** @brief TCP port of the primary sequencer's ER inbound listener. */
    uint16_t sequencer_er_port{7021};

    /** @brief Host address of the secondary sequencer's ER inbound listener. */
    std::string sequencer_er_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary sequencer's ER inbound listener. */
    uint16_t sequencer_er_secondary_port{7022};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

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

    // HA -- book replication (Slice A+B)
    //
    // When ha_enabled=true the ME runs as a primary/secondary pair.
    // Primary: processes orders, sends ERs, streams BookUpdate PDUs to
    //          the secondary over a dedicated outbound connection.
    // Secondary: receives BookUpdate PDUs from the primary and maintains
    //            a replica order book.  Does not process sequencer orders
    //            or send ERs until promoted (Slice C).

    /** @brief Enable HA book replication. Default false (single instance). */
    bool ha_enabled{false};

    /** @brief This instance's role: "primary" or "secondary". */
    std::string ha_role{"primary"};

    // Where this instance dials to reach its peer's replication listener. Both instances
    // dial and both listen: the connection that carries book updates is the one on which the
    // LEADER is the sender, and holding both permanently means a role change needs no
    // connection work at the moment the venue can least afford it.
    std::string peer_replication_host{"127.0.0.1"};
    uint16_t peer_replication_port{7026};

    // Secondary-side: inbound listener for book updates from primary.
    std::string replication_listen_host{"127.0.0.1"};
    uint16_t replication_listen_port{7026};

    // HA -- arbiter-mediated promotion and cancel-on-failover (Slice C+D)
    //
    // The secondary opens connections to the arbiter pool at startup and, on
    // loss of the primary replication channel, arms a promotion timer.  When it
    // fires it sends an ArbitrationReport to the arbiter and adopts leader or
    // follower based on the ArbitrationDecision.  The primary heartbeats the
    // arbiter to hold its lease.

    /** @brief Unique instance identity within the ME pair (1 = primary, 2 = secondary). */
    int32_t instance_id{1};

    /** @brief Host address of the primary arbiter's component listener. */
    std::string arbiter_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary arbiter's component listener. */
    uint16_t arbiter_primary_port{7200};

    /** @brief Host address of the secondary arbiter's component listener. */
    std::string arbiter_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary arbiter's component listener. */
    uint16_t arbiter_secondary_port{7200};

    /** @brief Promotion timeout: how long the secondary waits after losing the primary
     *  replication channel before requesting arbitration. Also the ceiling for
     *  arbiter reachability during promotion. */
    int32_t heartbeat_timeout_seconds{15};

    /** @brief Interval at which the primary heartbeats the arbiter pool to renew its lease. */
    int32_t heartbeat_interval_seconds{30};

    // Order book

    /** @brief Number of elements to pre-reserve in the order book hash map.
     *  Sets the bucket count via unordered_map::reserve() at startup so that
     *  no rehash occurs until the live order count exceeds this value.
     *  Size to the expected peak number of simultaneously live (non-terminal)
     *  orders.  Increase for load-test environments. */
    int32_t order_book_initial_capacity{1024};

    /** @brief Report a single order-book storage allocation at or above this many bytes.
     *  The book is a growing hash map on the OS heap, not a pool or slab, so the
     *  framework's handler_for_pool_exhausted never sees it -- it once reached 9.9 GB and
     *  the process was OOM-killed having logged no memory warning at all.  The map
     *  allocates its whole bucket array in one call and reallocates on each doubling, so a
     *  report is emitted roughly two dozen times over a process lifetime, never per order.
     *  64 MB by default: large enough to ignore ordinary sizing, small enough to give
     *  many doublings of warning before memory becomes a problem. */
    int64_t order_book_growth_report_threshold_bytes{64L * 1024 * 1024};

    // Wall clock

    /** @brief Clock used to generate transact_time on ExecutionReports when the
     *  inbound PDU does not carry a sequenced_at timestamp.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to produce deterministic timestamps in tests. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/design/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;
};

} // namespaces
