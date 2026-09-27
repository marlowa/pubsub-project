#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <LeaseTiming.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>

namespace arbiter {

/**
 * @brief Configuration for the arbiter process.
 *
 * The arbiter pool is the third voter in deciding which instance of each
 * component pair leads. It runs as a primary/secondary pair of arbiters, and
 * votes through whichever of them is active. An arbiter is active only while
 * a majority of the two arbiters and the witness has granted it a lease.
 *
 * Components connect to both arbiters and send each lease request to both.
 * The active arbiter answers with a grant or a refusal; the passive one stays
 * silent. See docs/availability/majority_leases.md.
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

    /** @brief This arbiter's identity: 1 for the primary, 2 for the secondary. The primary is preferred when both start together. */
    int32_t instance_id{1};

    /**
     * @brief Whether the venue is running as a high-availability deployment at all.
     *
     * Read from the one venue-wide `[ha] enabled` in the environment file, expanded into every
     * config that has an opinion about it. This component exists only to arbitrate between
     * instances, so when it is false the component refuses to start rather than sitting there
     * doing nothing: a running arbiter in a venue that has disowned arbitration is something an
     * operator will later trust. See docs/bug_list.md, BUG-0061.
     */
    bool ha_enabled{true};

    /**
     * @brief The peer arbiter's instance id.
     *
     * Configured rather than only learned, because it identifies the peer's vote before the two
     * have ever exchanged a message.
     */
    int32_t peer_instance_id{2};

    /** @brief Host address on which the peer listener binds for arbiter-to-arbiter PDUs. */
    std::string peer_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the peer listener binds (7203 primary, 7204 secondary). */
    uint16_t peer_listen_port{7203};

    /** @brief Host address of the peer arbiter's peer listener. */
    std::string peer_host{"127.0.0.1"};

    /** @brief TCP port of the peer arbiter's peer listener (7204 primary, 7203 secondary). */
    uint16_t peer_port{7204};

    // Witness -- the third voter in deciding which arbiter is active

    /** @brief Host address of the witness process. */
    std::string witness_host{"127.0.0.1"};

    /** @brief TCP port of the witness process. */
    uint16_t witness_port{7100};

    /**
     * @brief The timings of the leases that decide which arbiter is active, and that the active
     *        arbiter grants to component instances.
     *
     * Expanded from the environment's [shared] section, because every voter and every instance
     * holding a lease must use the same values. See fix_common/LeaseTiming.hpp.
     */
    fix_common::LeaseTiming lease{};

    /**
     * @brief File in which this arbiter records the promise of its vote in deciding which arbiter is
     * active, or that it is the active arbiter.
     *
     * Read at startup. A record written during the same boot of the machine lets an arbiter
     * restarted by its supervisor carry on from it, rather than wait out a lease period while the
     * other arbiter becomes active. See fix_common/LeasePromiseStore.hpp. Its directory must exist.
     */
    std::string lease_promise_file;

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
