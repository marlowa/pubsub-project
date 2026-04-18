#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "FixGatewayConfiguration.hpp"
#include "FixGatewayThread.hpp"

namespace sample_fix_gateway {

/**
 * @brief Top-level application class for the sample FIX 5.0SP2 gateway.
 *
 * SampleFixGateway owns all framework objects -- the logger, reactor, and
 * gateway thread -- and wires them together. It registers a single inbound
 * RawBytes listener on the configured host and port, registers the
 * FixGatewayThread with the reactor, and runs until a SIGTERM or SIGINT is
 * received or the reactor shuts down for any other reason.
 *
 * Usage:
 *   FixGatewayConfiguration config;
 *   config.logon_timeout = std::chrono::seconds{10};  // override defaults as needed
 *   SampleFixGateway gateway{config};
 *   return gateway.run();
 */
class SampleFixGateway {
public:
    /**
     * @brief Constructs the gateway with the given configuration.
     *
     * Creates the logger, reactor, and gateway thread. Registers the inbound
     * listener and the gateway thread with the reactor. Does not start the
     * reactor event loop -- call run() for that.
     *
     * @param[in] config Gateway configuration. The configuration is copied
     *                   and owned by this object.
     */
    explicit SampleFixGateway(const FixGatewayConfiguration& config);

    /**
     * @brief Starts the reactor event loop.
     *
     * Blocks until the reactor shuts down (e.g. on SIGTERM/SIGINT or
     * unrecoverable error).
     *
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

private:
    static const std::string log_file_name;

    FixGatewayConfiguration                       config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger>   logger_;
    pubsub_itc_fw::ServiceRegistry                service_registry_;
    pubsub_itc_fw::ReactorConfiguration           reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor>       reactor_;
    std::shared_ptr<FixGatewayThread>             gateway_thread_;
};

} // namespace sample_fix_gateway
