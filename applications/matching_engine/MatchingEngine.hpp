#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "MatchingEngineConfiguration.hpp"
#include "MatchingEngineThread.hpp"

namespace matching_engine {

/**
 * @brief Top-level application class for the matching engine.
 *
 * Wires together:
 *   - One inbound PDU listener for SequencedMessage PDUs from the sequencer.
 *   - One outbound PDU connection to the gateway for ExecutionReport PDUs.
 *     TODO: replace with pub/sub fanout when implemented.
 */
class MatchingEngine {
  public:
    /**
     * @param[in] config Matching engine configuration.
     */
    explicit MatchingEngine(const MatchingEngineConfiguration& config);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    static const std::string log_file_name;

    MatchingEngineConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<MatchingEngineThread> matching_engine_thread_;
};

} // namespace matching_engine
