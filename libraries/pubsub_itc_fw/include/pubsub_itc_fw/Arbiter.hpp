#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "ArbiterConfiguration.hpp"
#include "ArbiterThread.hpp"

namespace arbiter {

/**
 * @brief Top-level application class for the arbiter process.
 *
 * The arbiter manages the leadership-state map for component pairs
 * (sequencer pair, ME pair). It runs as a primary/secondary HA pair;
 * the witness resolves ties in the arbiter's own active/passive election.
 */
class Arbiter {
  public:
    /**
     * @param[in] config Arbiter configuration.
     * @param[in] logger Logger. Ownership transferred.
     */
    explicit Arbiter(const ArbiterConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run() const;

  private:
    ArbiterConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<ArbiterThread> arbiter_thread_;
};

} // namespaces
