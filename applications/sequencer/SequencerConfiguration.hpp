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

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};
};

} // namespace sequencer
