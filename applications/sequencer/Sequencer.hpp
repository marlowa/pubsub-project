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
 *   - One inbound PDU listener for ER PDUs from the matching engine.
 *   - One outbound PDU connection to the gateway for ER forwarding.
 *   - One outbound PDU connection to the peer sequencer instance.
 *   - One outbound PDU connection to the main-site arbiter.
 *
 * The logger is constructed in main() before the config is loaded.
 */
class Sequencer {
public:
    /**
     * @param[in] config Sequencer configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the
     *                   correct log levels applied from config.
     */
    explicit Sequencer(const SequencerConfiguration& config,
                       std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

private:
    SequencerConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<SequencerThread> sequencer_thread_;
};

} // namespace sequencer
