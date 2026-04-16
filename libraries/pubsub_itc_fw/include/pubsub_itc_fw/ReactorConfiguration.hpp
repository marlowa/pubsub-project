#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // For uint16_t

#include <chrono>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief Configuration data for the Reactor.
 *
 * This struct holds various settings that control the behavior of the Reactor,
 * such as the maximum number of events it should process in a single loop and
 * the port it should listen on.
 */
struct ReactorConfiguration {
    ReactorConfiguration() {
        // Change defaults to be more suitable for our command queue
        command_queue_config_.low_watermark = 2;
        command_queue_config_.high_watermark = 64;
        command_allocator_config_.objects_per_pool = 64;
        command_allocator_config_.initial_pools = 1;
    }

    size_t max_events_per_loop = 64; /**< The maximum number of events to handle in a single `epoll_wait` call. */

    /**
     * @brief The interval for checking for inactive threads and sockets.
     * This interval is also used to check if the reactor has been told to shutdown.
     */
    std::chrono::milliseconds inactivity_check_interval_{std::chrono::seconds{1}};

    /**
     * @brief The maximum allowed inactivity interval for inter-thread communication.
     */
    std::chrono::milliseconds itc_maximum_inactivity_interval_{std::chrono::seconds{60}};

    /**
     * @brief The maximum allowed inactivity interval for sockets.
     */
    std::chrono::milliseconds socket_maximum_inactivity_interval_{std::chrono::seconds{60}};

    // TODO need to revise what this is for this is how long we wait for INIT to complete
    // We probably need to add more smarts to the backstop check for this
    std::chrono::milliseconds init_phase_timeout_{std::chrono::seconds{10}};

    std::chrono::milliseconds shutdown_timeout_{std::chrono::seconds{1}};

    QueueConfiguration command_queue_config_;
    AllocatorConfiguration command_allocator_config_;

    /**
     * @brief The network endpoint on which this application instance listens for
     *        inbound TCP connections.
     *
     * This is the primary listening address for the process. All inbound traffic
     * (leader–follower protocol messages, FIX sessions, and other framework-based
     * clients) is accepted on this endpoint. The host may be IPv4, IPv6, or a DNS
     * name, and the port is the single TCP port owned by this reactor.
     */
    NetworkEndpointConfiguration primary_address;

    /**
     * @brief The network endpoint of the sibling node in the high-availability pair.
     *
     * This address identifies the other main instance in the HA configuration. It
     * is used by the leader–follower protocol to establish a control connection
     * for heartbeats, status exchange, and arbitration. It may also be used by
     * higher-level application logic to replicate state or traffic to a hot
     * standby, depending on the chosen replication strategy.
     */
    NetworkEndpointConfiguration secondary_address;

    /**
     * @brief The preferred DR (Disaster Recovery) arbitration endpoint.
     *
     * During leader arbitration, the node first attempts to contact this DR
     * instance to obtain an ArbitrationDecision. If this endpoint is unreachable,
     * the node will fall back to @ref dr_secondary_address.
     */
    NetworkEndpointConfiguration dr_primary_address;

    /**
     * @brief The fallback DR arbitration endpoint.
     *
     * If @ref dr_primary_address cannot be reached within the arbitration timeout,
     * the node attempts to contact this secondary DR instance. Only one DR node is
     * required to be reachable for arbitration to succeed.
     */
    NetworkEndpointConfiguration dr_secondary_address;

    /**
     * @brief Interval at which this node sends heartbeat messages to its peer.
     *
     * Heartbeats are the primary liveness signal between the two main nodes.
     * A shorter interval reduces failover latency but increases network traffic.
     * This value must be stable and consistent across both nodes.
     */
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{1}};

    /**
     * @brief Maximum time allowed without receiving a heartbeat from the peer.
     *
     * If this interval elapses without a heartbeat, the peer is considered
     * unresponsive and leader arbitration is initiated. This timeout must be
     * strictly greater than @ref heartbeat_interval to avoid false positives.
     */
    std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds{3}};

    /**
     * @brief Maximum time to wait for an ArbitrationDecision from a DR node.
     *
     * During leader election, the node contacts DR(primary) first and, if necessary,
     * DR(secondary). If neither responds within this timeout, the node proceeds using
     * fallback rules. This value bounds the worst‑case duration of an election.
     */
    std::chrono::milliseconds arbitration_timeout{std::chrono::seconds{2}};

    /**
     * @brief Maximum time to wait for a StatusResponse after sending a StatusQuery.
     *
     * StatusQuery is used during startup and recovery to determine the peer's
     * current role and state. If the peer does not respond within this interval,
     * it is treated as unavailable for the purposes of election and failover.
     */
    std::chrono::milliseconds status_query_timeout{std::chrono::seconds{1}};

    /**
     * @brief The 64-bit identifier for this node's primary instance.
     *
     * This value is included in every leader–follower protocol message. It must be
     * globally unique across all nodes participating in the cluster. Leadership is
     * determined deterministically by comparing instance IDs: the node with the
     * lowest instance ID becomes leader. This value must remain stable for the
     * lifetime of the process.
     */
    int64_t primary_instance_id{0};

    /**
     * @brief The expected 64-bit identifier of the peer node's primary instance.
     *
     * Incoming protocol messages include the sender's instance ID. This field is
     * used to validate that the peer is the node we expect to be paired with. A
     * mismatch indicates a configuration error or a stale/restarted peer.
     */
    int64_t secondary_instance_id{0};

    /**
     * @brief Maximum time to wait for a non-blocking TCP connect() to complete.
     *
     * If finish_connect() has not succeeded within this interval after connect()
     * was called, the reactor tears down the connection attempt and delivers a
     * ConnectionFailed event to the requesting ApplicationThread. This bounds
     * the worst-case connection setup time when the remote end is unreachable
     * or slow to respond.
     *
     * Default: 5 seconds — appropriate for LAN connections. Increase for WAN.
     */
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{5}};

    /**
     * @brief Size in bytes of each slab used by the reactor's inbound PDU slab allocator.
     *
     * The inbound allocator receives payload bytes directly from the socket into
     * slab-allocated chunks (zero-copy). This is a hard upper bound on the size of
     * any single inbound PDU payload — an attempt to receive a payload larger than
     * this value will throw PreconditionAssertion. This constraint is intentional:
     * it encourages application designers to decompose large responses into multiple
     * focused messages rather than sending unbounded payloads in a single PDU.
     *
     * The allocator grows automatically by chaining new slabs of this size when the
     * current slab is exhausted, so overall throughput is not limited — only the
     * size of any individual PDU payload.
     *
     * Default: 65536 bytes (64 KB).
     */
    size_t inbound_slab_size{65536};

};

} // namespace pubsub_itc_fw
