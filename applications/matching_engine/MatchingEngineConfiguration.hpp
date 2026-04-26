#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace matching_engine {

/**
 * @brief Configuration for the matching engine application.
 *
 * The matching engine accepts sequenced order PDUs from the sequencer,
 * matches them, and sends ExecutionReport PDUs back to the sequencer's
 * ER inbound listener. The sequencer then forwards ERs to the gateway.
 * All traffic flows through the sequencer in both directions.
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
    // Outbound -- ExecutionReport PDUs back to the sequencer
    //
    // The ME connects outbound to the sequencer's ER inbound listener.
    // The sequencer then forwards ERs to the appropriate gateway.
    // ----------------------------------------------------------------

    /** @brief Host address of the sequencer's ER inbound listener. */
    std::string sequencer_er_host{"127.0.0.1"};

    /** @brief TCP port of the sequencer's ER inbound listener. */
    uint16_t sequencer_er_port{7021};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};
};

} // namespace matching_engine
