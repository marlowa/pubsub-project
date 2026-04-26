#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "FixGatewaySeqConfiguration.hpp"
#include "FixGatewaySeqThread.hpp"

namespace sample_fix_gateway_seq {

/**
 * @brief Top-level application class for the sequencer-backed FIX gateway.
 *
 * Owns all framework objects and wires them together:
 *
 *   - One inbound RawBytes listener for FIX client connections.
 *   - Two outbound PDU connections to the primary and secondary sequencer.
 *   - One inbound PDU listener for ExecutionReport PDUs from the sequencer.
 *
 * The logger is constructed in main() before the config is loaded.
 */
class SampleFixGatewaySeq {
public:
    /**
     * @brief Constructs the gateway and wires all connections.
     * @param[in] config Gateway configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the
     *                   correct log levels applied from config.
     */
    explicit SampleFixGatewaySeq(const FixGatewaySeqConfiguration& config,
                                  std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

private:
    FixGatewaySeqConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<FixGatewaySeqThread> gateway_thread_;
};

} // namespace sample_fix_gateway_seq
