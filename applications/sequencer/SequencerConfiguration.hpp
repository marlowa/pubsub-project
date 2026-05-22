#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

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
 * HA: primary and secondary instances are managed by the leader-follower
 * protocol. A lightweight main-site arbiter resolves startup ties.
 * DR arbiters are not used for this topology.
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
    // HA -- leader-follower
    // ----------------------------------------------------------------

    /** @brief Unique integer identifier for this sequencer instance. Lowest wins. */
    int32_t instance_id{1};

    /** @brief Host address of the main-site arbiter. */
    std::string arbiter_host{"127.0.0.1"};

    /** @brief TCP port of the main-site arbiter. */
    uint16_t arbiter_port{7100};

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
    std::string fence_file_path{"/tmp/sequencer_fence"};

    // ----------------------------------------------------------------
    // WAL -- mmap'd on-disk write-ahead log
    // ----------------------------------------------------------------

    /** @brief Directory in which WAL segment files are created. */
    std::string wal_directory{"/tmp/sequencer_wal"};

    /** @brief Pre-allocation size of each WAL segment file in bytes. */
    size_t wal_segment_size{4 * 1024 * 1024};

    /** @brief How often the WAL snapshot is taken, in seconds. */
    int32_t snapshot_interval_seconds{30};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};
};

} // namespace sequencer
