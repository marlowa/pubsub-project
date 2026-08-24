#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>

namespace arbiter {

/**
 * @brief Configuration for the arbiter process.
 *
 * The arbiter manages the leadership-state map for component pairs
 * (sequencer pair, ME pair). It runs as a primary/secondary HA pair:
 * one instance is active (makes decisions), the other is passive
 * (replicates state). The witness resolves ties in the arbiter's own
 * election.
 *
 * Components (sequencer, ME) connect to both arbiter instances and
 * send heartbeats and lease-renewal requests. The active arbiter
 * sends back ArbitrationDecision PDUs. The passive arbiter drops
 * component requests with a log warning.
 *
 * See pubsub_itc_fw_topology.puml for the authoritative topology.
 */
struct ArbiterConfiguration {
    // Inbound -- component connections (sequencer pair, ME pair)

    /** @brief Host address on which the arbiter listens for component connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the arbiter listens for component connections. */
    uint16_t listen_port{7200};

    // HA -- arbiter identity and peer arbiter connection

    /** @brief Unique integer identifier for this arbiter instance. Lowest wins active role. */
    int32_t instance_id{1};

    /** @brief Host address on which the peer listener binds for arbiter-to-arbiter PDUs. */
    std::string peer_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the peer listener binds (7203 primary, 7204 secondary). */
    uint16_t peer_listen_port{7203};

    /** @brief Host address of the peer arbiter's peer listener. */
    std::string peer_host{"127.0.0.1"};

    /** @brief TCP port of the peer arbiter's peer listener (7204 primary, 7203 secondary). */
    uint16_t peer_port{7204};

    // Witness -- for arbiter-vs-arbiter tie-breaking

    /** @brief Host address of the witness process. */
    std::string witness_host{"127.0.0.1"};

    /** @brief TCP port of the witness process. */
    uint16_t witness_port{7100};

    // Timing

    /** @brief How often this arbiter sends Heartbeat PDUs to the peer, in seconds. */
    int32_t heartbeat_interval_seconds{5};

    /**
     * @brief How long to wait at startup for a peer to appear before self-promoting to active.
     *
     * Should be long enough that both arbiters can connect and exchange StatusQuery
     * before either self-promotes, but short enough that the system becomes operational
     * quickly when the peer is genuinely absent.
     */
    int32_t startup_election_timeout_seconds{20};

    /** @brief How long without a peer Heartbeat before the passive arbiter promotes itself. */
    int32_t heartbeat_timeout_seconds{15};

    /**
     * @brief How long to wait for an ArbiterVoteResponse from the witness before
     * self-promoting using the local instance-id rule (degraded mode).
     */
    int32_t vote_timeout_seconds{3};

    /** @brief How often this arbiter sends ArbiterHeartbeat PDUs to the witness, in seconds. */
    int32_t witness_heartbeat_interval_seconds{30};

    // Logging

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    // Reactor

    /** @brief Enable CPU core pinning for registered application threads. */
    bool cpu_pinning_enabled;

    /** @brief Exclude CPU 0 from pinning candidates (for machines without isolated cores). */
    bool cpu_pinning_reserve_cpu0;

    /** @brief Path to the shared CPU registry file, under the deployment's run
     *  directory. Mandatory whenever cpu_pinning_enabled is true. */
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

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t event_queue_pool_objects_per_slab{64};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // Command queue pool  (Reactor ReactorControlCommand outbound queue)

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t command_queue_pool_objects_per_slab{64};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/operations/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;
};

} // namespaces
