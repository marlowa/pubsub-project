#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

namespace sequencer {

/**
 * @brief Configuration for the sequencer application.
 *
 * The sequencer accepts inbound PDU connections from gateways, stamps a
 * monotonically increasing sequence number onto each PDU, wraps it in a
 * SequencedMessage envelope, and forwards it to the matching engine.
 * Only the leader forwards; the follower receives but discards, staying
 * in sync so that failover is gap-free.
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
    // Outbound -- matching engine
    // ----------------------------------------------------------------

    /** @brief Host address of the matching engine. */
    std::string matching_engine_host{"127.0.0.1"};

    /** @brief TCP port of the matching engine. */
    uint16_t matching_engine_port{7020};

    // ----------------------------------------------------------------
    // HA -- leader-follower
    // ----------------------------------------------------------------

    /** @brief Unique integer identifier for this sequencer instance. Lowest wins. */
    int32_t instance_id{1};

    /** @brief Host address of the peer sequencer instance. */
    std::string peer_host{"127.0.0.1"};

    /** @brief TCP port of the peer sequencer instance. */
    uint16_t peer_port{7003};

    /** @brief Host address of the main-site arbiter. */
    std::string arbiter_host{"127.0.0.1"};

    /** @brief TCP port of the main-site arbiter. */
    uint16_t arbiter_port{7100};
};

} // namespace sequencer
