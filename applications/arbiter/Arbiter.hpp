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
 * @brief Top-level application class for the main-site arbiter.
 *
 * The arbiter has no involvement in the order flow. It listens for inbound
 * PDU connections from sequencer instances and implements the arbiter side
 * of the leader-follower protocol only.
 *
 * The logger is constructed in main() before the config is loaded, so that
 * config errors can be logged rather than only printed to stderr.
 */
class Arbiter {
public:
    /**
     * @param[in] config Arbiter configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the
     *                   correct log levels applied from config.
     */
    explicit Arbiter(const ArbiterConfiguration& config,
                     std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

private:
    ArbiterConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<ArbiterThread> arbiter_thread_;
};

} // namespace arbiter
