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
 *   - One inbound PDU listener for ExecutionReport PDUs from the matching
 *     engine (stub for future pub/sub fanout).
 *
 * The gateway sends every order PDU to both sequencer instances so that the
 * follower stays in sync with the leader and failover is gap-free.
 */
class SampleFixGatewaySeq {
  public:
    /**
     * @brief Constructs the gateway and wires all connections.
     * @param[in] config Gateway configuration.
     */
    explicit SampleFixGatewaySeq(const FixGatewaySeqConfiguration& config);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    static const std::string log_file_name;

    FixGatewaySeqConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<FixGatewaySeqThread> gateway_thread_;
};

} // namespace sample_fix_gateway_seq
