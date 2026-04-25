#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

namespace matching_engine {

/**
 * @brief Configuration for the matching engine application.
 *
 * The matching engine accepts sequenced order PDUs from the sequencer,
 * matches them, and sends ExecutionReport PDUs back to each gateway via
 * direct TCP PDU connections (stub for future pub/sub fanout).
 */
struct MatchingEngineConfiguration {
    // ----------------------------------------------------------------
    // Inbound -- sequenced order PDUs from the sequencer
    // ----------------------------------------------------------------

    /** @brief Host address on which the ME listens for PDUs from the sequencer. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the ME listens for PDUs from the sequencer. */
    uint16_t listen_port{7020};

    // ----------------------------------------------------------------
    // Outbound -- ExecutionReport PDUs to the gateway
    //
    // Direct TCP PDU connection to each gateway.
    // TODO: replace with pub/sub fanout when implemented.
    // ----------------------------------------------------------------

    /** @brief Host address of the gateway to send ExecutionReport PDUs to. */
    std::string gateway_host{"127.0.0.1"};

    /** @brief TCP port of the gateway ER listener. */
    uint16_t gateway_port{7010};
};

} // namespace matching_engine
