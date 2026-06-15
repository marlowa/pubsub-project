#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "MatchingEnginePublisherConfiguration.hpp"
#include "MatchingEnginePublisherThread.hpp"

namespace matching_engine_publisher {

/**
 * @brief Top-level application class for the Matching Engine Publisher.
 *
 * Registers inbound listeners (orders topic, ER topic, HA peer), populates
 * the ServiceRegistry with outbound connection targets (both sequencer WAL
 * listeners, peer, arbiters), creates the MatchingEnginePublisherThread, and
 * runs the reactor event loop.
 */
class MatchingEnginePublisher {
  public:
    explicit MatchingEnginePublisher(MatchingEnginePublisherConfiguration config,
                                     std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    int run() const;

  private:
    MatchingEnginePublisherConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<MatchingEnginePublisherThread> thread_;
};

} // namespaces
