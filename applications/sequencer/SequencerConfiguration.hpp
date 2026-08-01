#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>
#include <vector>

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
    // Inbound -- gateway order PDUs

    /** @brief Host address on which the sequencer listens for gateway PDUs. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the sequencer listens for gateway PDUs. */
    uint16_t listen_port{7001};

    // Inbound -- ExecutionReport PDUs from the matching engine

    /** @brief Host address on which the sequencer listens for ER PDUs from the ME. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the sequencer listens for ER PDUs from the ME. */
    uint16_t er_listen_port{7021};

    // Outbound -- gateway ER forwarding
    //
    // A venue runs more than one gateway: the ASCII FIX one and the binary one speak
    // different client protocols into the same book, and each may run as several
    // instances for availability. The sequencer dials every configured endpoint and
    // routes each ER back to the one the order came from.
    //
    // A session connection id is only unique within a single gateway process, so it is
    // the triple (protocol, instance, connection) that identifies a session venue-wide.
    // This collection supplies the first two: protocol says which wire format the report
    // is encoded in, instance says which process to send it to.
    //
    // See docs/design/gateway_ha.md.

    /** @brief One gateway process the sequencer delivers execution reports to. */
    struct GatewayEndpoint {
        /** @brief Which client protocol, from gateway_ids: fix_order_gateway or binary_order_gateway. */
        int16_t protocol{1};

        /** @brief Which instance of that protocol, numbered from 1 within the protocol. */
        int16_t instance{1};

        /** @brief Host address of this gateway's ER inbound listener. */
        std::string host{"127.0.0.1"};

        /** @brief TCP port of this gateway's ER inbound listener. */
        uint16_t port{7010};

        /**
         * @brief Reactor service name for this endpoint, e.g. "gateway_1_2".
         *
         * Unique per (protocol, instance) so the two axes cannot alias: instance 1 of the
         * FIX gateway and instance 1 of the binary gateway are different processes.
         */
        [[nodiscard]] std::string service_name() const {
            return "gateway_" + std::to_string(protocol) + "_" + std::to_string(instance);
        }
    };

    /**
     * @brief Every gateway process to deliver execution reports to.
     *
     * Replaces the earlier scalar gateway_host/gateway_port pair and the
     * binary_order_gateway_enabled flag: a gateway that is not deployed simply has no entry,
     * which says the same thing without a separate switch.
     */
    std::vector<GatewayEndpoint> gateway_endpoints{};

    // Outbound -- matching engine order forwarding
    //
    // The sequencer connects outbound to the ME's order listener and
    // forwards sequenced order PDUs over that connection.

    /** @brief Host address of the matching engine order inbound listener. */
    std::string matching_engine_host{"127.0.0.1"};

    /** @brief TCP port of the matching engine order inbound listener. */
    uint16_t matching_engine_port{7020};

    /**
     * @brief Host address of the ME-secondary order inbound listener.
     *
     * Only used when ha_enabled=true. The sequencer keeps a pre-warmed standby
     * connection to ME-secondary. On ME failover the secondary sends a
     * MePositionRequest over this connection; the sequencer replays the WAL
     * catch-up and then promotes the standby to the active ME order connection.
     */
    std::string matching_engine_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the ME-secondary order inbound listener (ha_enabled only). */
    uint16_t matching_engine_secondary_port{7023};

    // HA -- leader-follower via arbiter pool

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

    // HA mode -- when false, the sequencer starts as leader immediately
    // with no peer election. Set to true only when running a paired
    // primary + secondary deployment.

    /**
     * @brief Enable leader-follower HA. When false (default), the sequencer
     * starts as leader immediately and skips all peer/election machinery.
     * When true, the peer election protocol runs and a secondary sequencer
     * is expected.
     */
    bool ha_enabled{false};

    // Peer -- sequencer-to-sequencer leader-follower protocol (slice 6)
    //
    // Each sequencer binds a dedicated listener for peer PDUs and connects
    // outbound to the other sequencer's peer listener. Primary listens on
    // 7003 and connects to 7004; secondary listens on 7004 and connects to
    // 7003. The heartbeat mechanism is used for liveness detection and
    // leader election.

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
     * Should be short (>= connect_retry_interval) so that single-node deployments
     * become operational quickly without waiting for the full heartbeat timeout.
     *
     * Default: 3 seconds (allows one connection retry cycle on the peer side).
     */
    int32_t startup_election_timeout_seconds{3};

    /** @brief How long without a Heartbeat before the follower promotes itself, in seconds. */
    int32_t heartbeat_timeout_seconds{15};

    // WAL -- mmap'd on-disk write-ahead log

    /** @brief Directory in which WAL segment files are created. */
    std::string wal_directory{"/var/tmp/pubsub/sequencer_wal"};

    /** @brief Pre-allocation size of each WAL segment file in bytes. */
    size_t wal_segment_size{4 * 1024 * 1024};

    /** @brief How often the WAL snapshot is taken, in seconds. */
    int32_t snapshot_interval_seconds{30};

    // External WAL subscriber listener (MEP primary and secondary connect here)

    /** @brief Host address on which the external WAL subscriber listener binds. */
    std::string wal_subscriber_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the external WAL subscriber listener binds (7030 primary, 7031 secondary). */
    uint16_t wal_subscriber_listen_port{7030};

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

    // Wall clock

    /** @brief Clock used to stamp sequenced_at on outbound NOS and OCR PDUs.
     *  Defaults to SystemWallClock (real UTC wall time). Inject a ReplayClock
     *  to drive timestamps from WAL records during replay. */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    // Replay mode  (set by the --replay command-line flag)

    /** @brief When true, the sequencer reads the WAL and replays all records
     *  to the matching engine instead of accepting live gateway connections.
     *  HA, gateway, arbiter, and peer connections are skipped.  The WAL
     *  snapshot timer is suppressed.  Set via the --replay command-line flag. */
    bool replay_mode{false};
};

} // namespaces
