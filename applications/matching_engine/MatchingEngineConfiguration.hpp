#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/TestEventsConfiguration.hpp>
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

    /** @brief How long the reactor looks for work before sleeping, or zero to sleep at once.
     *
     *  Read from a duration written with its unit, such as "50ms".
     *
     *  Optional, and zero where the file says nothing, which is the behaviour without it.
     *  Setting it uses a whole core whether or not orders arrive, so it belongs only on the
     *  components of the order path and only where the venue is quiet between orders. See
     *  ReactorConfiguration::spin_before_block. */
    std::chrono::microseconds reactor_spin_before_block{0};

    /**
     * @brief Quiet spins between one look for work and the next, while the reactor is looking
     *        for work rather than sleeping on it.
     *
     * Negative means the deployed file did not say, and the framework's own default is used.
     * That is why this is not initialised to the default itself: a second copy of the number
     * here could drift away from ReactorConfiguration::quiet_spins_between_polls without
     * anything failing, and the symptom would be a component quietly polling at a rate nobody
     * chose.
     */
    int32_t reactor_quiet_spins_between_polls{-1};

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

    // High availability. When ha_enabled is true the matching engine runs as one of a pair. The instance that leads is
    // sent commands by order routing and sends the reports; the other follows the WAL, applying every command and
    // keeping its reports until the WAL holds the leader's (docs/availability/matching_engine_lockstep.md). The two are
    // linked by [peer], which carries the lease requests between them, their role announcements and their settings.

    /** @brief Whether this instance is one of a pair. Default false (a single instance). */
    bool ha_enabled{false};

    /** @brief This instance's role: "primary" or "secondary". */
    std::string ha_role{"primary"};

    // Where this instance dials to reach its peer's listener ([peer] host and port). Both instances dial and both listen,
    // and holding both connections permanently means a role change needs no connection work at the moment the venue can
    // least afford it.
    std::string peer_host{"127.0.0.1"};
    uint16_t peer_port{7026};

    // Where this instance listens for its peer's connection ([peer] listen_host and listen_port).
    std::string peer_listen_host{"127.0.0.1"};
    uint16_t peer_listen_port{7026};

    // HA -- which instance leads, and cancel-on-failover
    //
    // An instance leads only while a majority of three voters -- itself, its peer and the arbiter
    // pool -- has granted it a lease that has not run out. Both instances connect to both arbiters,
    // and ask their peer over the connections to it ([peer]). See docs/availability/majority_leases.md.

    /** @brief Unique instance identity within the ME pair (1 = primary, 2 = secondary). */
    int32_t instance_id{1};

    /// File holding the leadership epoch across restarts. See pubsub_itc_fw::EpochStore.
    std::string epoch_state_file;

    /** @brief Host address of the primary arbiter's component listener. */
    std::string arbiter_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary arbiter's component listener. */
    uint16_t arbiter_primary_port{7200};

    /** @brief Host address of the secondary arbiter's component listener. */
    std::string arbiter_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary arbiter's component listener. */
    uint16_t arbiter_secondary_port{7200};

    /**
     * @brief The timings of the leases that decide which matching engine leads.
     *
     * Expanded from the environment's [shared] section, because every voter and every instance
     * holding a lease must use the same values. See pubsub_itc_fw/LeaseTiming.hpp.
     */
    pubsub_itc_fw::LeaseTiming lease{};

    /**
     * @brief How long an instance catching up on the sequencer's record waits, with nothing arriving,
     *        before asking again. A sequencer that is not leading drops the request without answering.
     */
    int32_t catch_up_retry_seconds{15};

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

    /** @brief File holding the open orders, so that they survive this process dying.
     *
     *  A memory-mapped region of fixed-size records, one for each order currently open. It is
     *  what makes an order placed before a restart still open, and still cancellable, after
     *  one. Put it where a restart will find it and a reboot need not: it survives the process
     *  dying rather than the machine dying, which is what the second machine is for.
     *
     *  See docs/durability/open_order_checkpoint.md. */
    std::string order_book_region_path{"var/matching_engine/open_orders.region"};

    /** @brief The most orders that may be open at once.
     *
     *  Fixes the size of the region, which cannot grow: an order arriving when every record is
     *  taken is refused, because the venue must not accept an order it could not account for
     *  after a restart. Size it to the peak simultaneously open orders and not to the day's
     *  volume -- an order that has been cancelled or filled releases its record.
     *
     *  A million costs roughly 320 MB of address space, which is not resident until touched. */
    int32_t order_book_region_capacity{1000000};

    /** @brief The file holding the secret the engine's OrderID and ExecID keys are derived from, as 64 hexadecimal digits.
     *
     *  Both engines of a pair must hold the same secret, or they would name the same report
     *  differently; each tells the other a fingerprint of it when they connect, and the one that
     *  leads bars the other if they differ. Support staff hold it too, to turn an identifier a
     *  member quotes back into its position in the WAL with identifier_to_position. Keep it
     *  readable by the engine's user alone, as the venue's TLS keys are. See
     *  docs/availability/deterministic_matching_engine.md, section 3.3. */
    std::string identifier_secret_file;

    // Wall clock

    /** @brief Clock used to generate transact_time on ExecutionReports when the
     *  inbound PDU does not carry a sequenced_at timestamp.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to produce deterministic timestamps in tests. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/operations/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;

    /**
     * @brief Where to send test events, from the optional [test_events] section; see docs/availability/test_events.md.
     *
     * Only a test writes the section. Without it, no test events are sent and no connection is opened for them.
     */
    pubsub_itc_fw::TestEventsConfiguration test_events;

    /** @brief Bucket bounds in nanoseconds for order_path_elapsed_nanoseconds, ascending.
     *
     *  Empty when metrics are disabled, in which case nothing registers and it is unused.
     *
     *  **Must match every other component on the order path exactly**, because the metric is
     *  read as the difference between checkpoints recorded by different processes. See
     *  OrderPathMetrics.hpp. */
    std::vector<double> order_path_elapsed_buckets;
};

} // namespaces
