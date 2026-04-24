#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Configuration data for the high-availability (HA) subsystem.
 *
 * This struct holds the static configuration for the leader-follower protocol,
 * heartbeat mechanism, and DR arbitration. It is intentionally independent of
 * ReactorConfiguration -- the HA layer sits above the reactor and uses it as
 * a transport. Neither the Reactor nor any of its managers reference this struct
 * directly.
 *
 * Primary and secondary are fixed by configuration and never change. Leader and
 * follower are roles that can move between instances. In normal operation the
 * primary is the leader and the secondary is the follower.
 *
 * Note: arbitration logic for machine death and network partition scenarios is
 * not yet fully designed. This struct will grow as the HA design matures.
 */
struct HAConfiguration {
    // ----------------------------------------------------------------
    // Node identity
    // ----------------------------------------------------------------

    /**
     * @brief The 64-bit identifier for this node's primary instance.
     *
     * This value is included in every leader-follower protocol message. It must
     * be globally unique across all nodes participating in the cluster. Leadership
     * is determined deterministically by comparing instance IDs: the node with the
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

    // ----------------------------------------------------------------
    // Network endpoints
    // ----------------------------------------------------------------

    /**
     * @brief The network endpoint on which this application instance listens for
     *        inbound TCP connections.
     *
     * This is the primary listening address for the process. All inbound traffic
     * (leader-follower protocol messages, FIX sessions, and other framework-based
     * clients) is accepted on this endpoint.
     */
    NetworkEndpointConfiguration primary_address;

    /**
     * @brief The network endpoint of the sibling node in the high-availability pair.
     *
     * Used by the leader-follower protocol to establish a control connection for
     * heartbeats, status exchange, and arbitration.
     */
    NetworkEndpointConfiguration secondary_address;

    /**
     * @brief The preferred DR (Disaster Recovery) arbitration endpoint.
     *
     * During leader arbitration, the node first attempts to contact this DR
     * instance to obtain an ArbitrationDecision. If this endpoint is unreachable,
     * the node falls back to dr_secondary_address.
     */
    NetworkEndpointConfiguration dr_primary_address;

    /**
     * @brief The fallback DR arbitration endpoint.
     *
     * If dr_primary_address cannot be reached within the arbitration timeout,
     * the node attempts to contact this secondary DR instance. Only one DR node
     * is required to be reachable for arbitration to succeed.
     */
    NetworkEndpointConfiguration dr_secondary_address;

    // ----------------------------------------------------------------
    // Heartbeat settings
    // ----------------------------------------------------------------

    /**
     * @brief Interval at which this node sends heartbeat messages to its peer.
     *
     * A shorter interval reduces failover latency but increases network traffic.
     * This value must be consistent across both nodes.
     */
    std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{1}};

    /**
     * @brief Maximum time allowed without receiving a heartbeat from the peer.
     *
     * If this interval elapses without a heartbeat, the peer is considered
     * unresponsive and leader arbitration is initiated. Must be strictly greater
     * than heartbeat_interval to avoid false positives.
     */
    std::chrono::milliseconds heartbeat_timeout{std::chrono::seconds{3}};

    // ----------------------------------------------------------------
    // Arbitration and status query timeouts
    // ----------------------------------------------------------------

    /**
     * @brief Maximum time to wait for an ArbitrationDecision from a DR node.
     *
     * During leader election, the node contacts dr_primary_address first and,
     * if necessary, dr_secondary_address. If neither responds within this timeout
     * the node proceeds using fallback rules. This value bounds the worst-case
     * duration of an election.
     */
    std::chrono::milliseconds arbitration_timeout{std::chrono::seconds{2}};

    /**
     * @brief Maximum time to wait for a StatusResponse after sending a StatusQuery.
     *
     * StatusQuery is used during startup and recovery to determine the peer's
     * current role and state. If the peer does not respond within this interval
     * it is treated as unavailable for the purposes of election and failover.
     */
    std::chrono::milliseconds status_query_timeout{std::chrono::seconds{1}};
};

} // namespace pubsub_itc_fw
