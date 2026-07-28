#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
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
 *   - One outbound PDU connection to the sequencer ER listener.
 *
 * The logger is constructed in main() before the config is loaded.
 */
class MatchingEngine {
  public:
    /// Reactor thread plus the one MatchingEngineThread registered in the constructor.
    /// Answered to deploy.py via --hot-path-thread-count and checked against the
    /// real registrations at startup; see HotPathThreadCount.hpp.
    static constexpr size_t hot_path_thread_count = 2;

    /**
     * @param[in] config Matching engine configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the
     *                   correct log levels applied from config.
     */
    explicit MatchingEngine(MatchingEngineConfiguration config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    MatchingEngineConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<MatchingEngineThread> matching_engine_thread_;
};

} // namespaces
