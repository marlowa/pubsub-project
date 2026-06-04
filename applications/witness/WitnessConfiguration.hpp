#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace witness {

/**
 * @brief Configuration for the witness process.
 *
 * The witness is a lightweight process that implements only the witness side
 * of the leader-follower protocol. It has no involvement in the order flow.
 * It listens for connections from both sequencer instances, receives
 * ArbitrationReport PDUs, and replies with ArbitrationDecision PDUs to
 * break leader-election ties.
 */
struct WitnessConfiguration {
    /** @brief Host address on which the witness listens for sequencer connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the witness listens. */
    uint16_t listen_port{7100};

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
    int32_t event_queue_pool_objects_per_slab{16};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // ----------------------------------------------------------------
    // Command queue pool  (Reactor ReactorControlCommand outbound queue)
    // ----------------------------------------------------------------

    /** @brief Number of objects in each fixed-size memory pool slab.
     *  Increase if command-queue pool-exhaustion warnings appear in the log. */
    int32_t command_queue_pool_objects_per_slab{16};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};
};

} // namespace witness
