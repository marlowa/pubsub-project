#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

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
};

} // namespace arbiter
