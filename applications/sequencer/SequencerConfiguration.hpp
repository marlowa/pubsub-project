#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace sequencer {

/**
 * @brief Configuration for the sequencer application.
 *
 * The sequencer accepts inbound PDU connections from gateways, stamps a
 * monotonically increasing sequence number onto each PDU, wraps it in a
 * SequencedMessage envelope, and forwards it to the matching engine.
 * The ME sends ExecutionReport PDUs back to the sequencer, which forwards
 * them to the originating gateway. Only the leader forwards; the follower
 * receives but discards, staying in sync so that failover is gap-free.
 *
 * HA: primary and secondary instances are managed by the arbiter pool
 * (arbiter-primary, arbiter-secondary, witness). Each sequencer connects
 * to both arbiter instances. The arbiter makes authoritative leadership
 * decisions; the witness resolves ties within the arbiter pair itself.
 *
 * See pubsub_itc_fw_topology.puml for the authoritative topology.
 */
struct SequencerConfiguration {
    // ----------------------------------------------------------------
    // Inbound -- gateway order PDUs
    // ----------------------------------------------------------------

    /** @brief Host address on which the sequencer listens for gateway PDUs. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the sequencer listens for gateway PDUs. */
    uint16_t listen_port{7001};

    // ----------------------------------------------------------------
    // Inbound -- ExecutionReport PDUs from the matching engine
    // ----------------------------------------------------------------

    /** @brief Host address on which the sequencer listens for ER PDUs from the ME. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the sequencer listens for ER PDUs from the ME. */
    uint16_t er_listen_port{7021};

    // ----------------------------------------------------------------
    // Outbound -- gateway ER forwarding
    // ----------------------------------------------------------------

    /** @brief Host address of the gateway ER inbound listener. */
    std::string gateway_host{"127.0.0.1"};

    /** @brief TCP port of the gateway ER inbound listener. */
    uint16_t gateway_port{7010};

    // ----------------------------------------------------------------
    // Outbound -- matching engine order forwarding
    //
    // The sequencer connects outbound to the ME's order listener and
    // forwards sequenced order PDUs over that connection.
    // ----------------------------------------------------------------

    /** @brief Host address of the matching engine order inbound listener. */
    std::string matching_engine_host{"127.0.0.1"};

    /** @brief TCP port of the matching engine order inbound listener. */
    uint16_t matching_engine_port{7020};

    // ----------------------------------------------------------------
    // HA -- leader-follower via arbiter pool
    // ----------------------------------------------------------------

    /** @brief Unique integer identifier for this sequencer instance. Lowest wins. */
    int32_t instance_id{1};

    /** @brief Host address of the primary arbiter's component listener. */
    std::string arbiter_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary arbiter's component listener. */
    uint16_t arbiter_primary_port{7200};

    /** @brief Host address of the secondary arbiter's component listener. */
    std::string arbiter_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary arbiter's component listener. */
    uint16_t arbiter_secondary_port{7201};

    /**
     * @brief How long to wait for an ArbitrationDecision from the active arbiter
     * before self-promoting using the local instance-id rule (degraded mode).
     * Only applies when ha_enabled=true.
     */
    int32_t arbitration_timeout_seconds{3};

    // ----------------------------------------------------------------
    // HA mode -- when false, the sequencer starts as leader immediately
    // with no peer election. Set to true only when running a paired
    // primary + secondary deployment.
    // ----------------------------------------------------------------

    /**
     * @brief Enable leader-follower HA. When false (default), the sequencer
     * starts as leader immediately and skips all peer/election machinery.
     * When true, the peer election protocol runs and a secondary sequencer
     * is expected.
     */
    bool ha_enabled{false};

    // ----------------------------------------------------------------
    // Peer -- sequencer-to-sequencer leader-follower protocol (slice 6)
    //
    // Each sequencer binds a dedicated listener for peer PDUs and connects
    // outbound to the other sequencer's peer listener. Primary listens on
    // 7003 and connects to 7004; secondary listens on 7004 and connects to
    // 7003. The heartbeat mechanism is used for liveness detection and
    // leader election.
    // ----------------------------------------------------------------

    /** @brief Host address on which the peer PDU listener binds. */
    std::string peer_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the peer PDU listener binds (7003 primary, 7004 secondary). */
    uint16_t peer_listen_port{7003};

    /** @brief Host address of the peer sequencer's peer listener. */
    std::string peer_host{"127.0.0.1"};

    /** @brief TCP port of the peer sequencer's peer listener (7004 primary, 7003 secondary). */
    uint16_t peer_port{7004};

    /** @brief How often this node sends Heartbeat PDUs to the peer, in seconds. */
    int32_t heartbeat_interval_seconds{5};

    /**
     * @brief How long to wait at startup for a peer to appear before self-promoting to leader.
     *
     * This is the initial election window: if no peer contact is made within this
     * many seconds of startup, the node unilaterally promotes itself to leader.
     * Should be short (≥ connect_retry_interval) so that single-node deployments
     * become operational quickly without waiting for the full heartbeat timeout.
     *
     * Default: 3 seconds (allows one connection retry cycle on the peer side).
     */
    int32_t startup_election_timeout_seconds{3};

    /** @brief How long without a Heartbeat before the follower promotes itself, in seconds. */
    int32_t heartbeat_timeout_seconds{15};

    /** @brief Path to the file written when this node promotes itself to leader. */
    std::string fence_file_path{"/dev/shm/sequencer_fence"};

    // ----------------------------------------------------------------
    // WAL -- mmap'd on-disk write-ahead log
    // ----------------------------------------------------------------

    /** @brief Directory in which WAL segment files are created. */
    std::string wal_directory{"/var/tmp/pubsub/sequencer_wal"};

    /** @brief Pre-allocation size of each WAL segment file in bytes. */
    size_t wal_segment_size{4 * 1024 * 1024};

    /** @brief How often the WAL snapshot is taken, in seconds. */
    int32_t snapshot_interval_seconds{30};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

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

    /** @brief How long to wait between "still disconnected" log warnings during outbound retry. */
    std::chrono::milliseconds connect_retry_warning_interval;

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
    // Wall clock
    // ----------------------------------------------------------------

    /** @brief Clock used to stamp sequenced_at on outbound NOS and OCR PDUs.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to drive timestamps from WAL records during replay. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    // ----------------------------------------------------------------
    // Replay mode  (set by the --replay command-line flag)
    // ----------------------------------------------------------------

    /** @brief When true, the sequencer reads the WAL and replays all records
     *  to the matching engine instead of accepting live gateway connections.
     *  HA, gateway, arbiter, and peer connections are skipped.  The WAL
     *  snapshot timer is suppressed.  Set via the --replay command-line flag. */
    bool replay_mode{false};
};

} // namespace sequencer
