#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "WitnessConfiguration.hpp"
#include "WitnessThread.hpp"

namespace witness {

/**
 * @brief Top-level application class for the witness process.
 *
 * The witness has no involvement in the order flow. It listens for inbound
 * PDU connections from sequencer instances and implements the witness side
 * of the leader-follower protocol only.
 */
class Witness {
  public:
    /**
     * @param[in] config Witness configuration.
     * @param[in] logger Logger. Ownership transferred.
     */
    explicit Witness(const WitnessConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    WitnessConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<WitnessThread> witness_thread_;
};

} // namespaces
