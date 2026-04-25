#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "SequencerConfiguration.hpp"
#include "SequencerThread.hpp"

namespace sequencer {

/**
 * @brief Top-level application class for the sequencer.
 *
 * Wires together:
 *   - One inbound PDU listener for order PDUs from gateways.
 *   - One outbound PDU connection to the matching engine (leader only forwards).
 *   - One outbound PDU connection to the peer sequencer instance.
 *   - One outbound PDU connection to the main-site arbiter.
 */
class Sequencer {
  public:
    /**
     * @param[in] config Sequencer configuration.
     */
    explicit Sequencer(const SequencerConfiguration& config);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    static const std::string log_file_name;

    SequencerConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<SequencerThread> sequencer_thread_;
};

} // namespace sequencer
