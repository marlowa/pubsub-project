#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace arbiter {

/**
 * @brief Configuration for the main-site arbiter.
 *
 * The arbiter is a lightweight process that implements only the arbiter side
 * of the leader-follower protocol. It has no involvement in the order flow.
 * It listens for ArbitrationReport PDUs from sequencer instances and replies
 * with ArbitrationDecision PDUs.
 *
 * Only a main-site arbiter is used for the sequencer HA topology.
 * DR arbiters are not used.
 */
struct ArbiterConfiguration {
    /** @brief Host address on which the arbiter listens for sequencer connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the arbiter listens. */
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
    bool cpu_pinning_dev_mode;

    /** @brief Path to the flock file used to serialise cross-process CPU registry access.
     *  Prefer /dev/shm/ so the file is cleared on reboot.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    std::string cpu_registry_lock_file;
};

} // namespace arbiter
