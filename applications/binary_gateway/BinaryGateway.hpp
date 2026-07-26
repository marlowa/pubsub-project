#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "BinaryGatewayConfiguration.hpp"
#include "BinaryGatewayThread.hpp"

namespace binary_gateway {

/**
 * @brief Top-level application class for the binary gateway.
 *
 * The order gateway's peer, offering the same venue over the internal PDU protocol
 * instead of ASCII FIX. Owns the framework objects and wires them together:
 *
 *   - One inbound FrameworkPdu listener for binary client connections.
 *   - One inbound FrameworkPdu listener for ExecutionReports from the sequencer.
 *   - Outbound PDU connections to the sequencer instances.
 *
 * Note the client listener is FrameworkPdu, not RawBytes as the FIX gateway's is:
 * clients speak the framed PDU protocol, so the framework does the framing and there
 * is no byte-stream parsing to do.
 */
class BinaryGateway {
  public:
    /**
     * @brief Constructs the gateway and wires all connections.
     * @param[in] config Gateway configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the correct
     *                   log levels applied from config.
     */
    BinaryGateway(const BinaryGatewayConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    BinaryGatewayConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<BinaryGatewayThread> gateway_thread_;
};

} // namespaces
