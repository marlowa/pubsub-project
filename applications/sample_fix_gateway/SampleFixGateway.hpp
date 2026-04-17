#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>

#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include "FixGatewayThread.hpp"

namespace sample_fix_gateway {

/**
 * @brief Top-level application class for the sample FIX 5.0SP2 gateway.
 *
 * SampleFixGateway owns all framework objects -- the logger, reactor, and
 * gateway thread -- and wires them together. It registers a single inbound
 * RawBytes listener on the configured host and port, registers the
 * FixGatewayThread with the reactor, and runs until a SIGTERM is received
 * or the reactor shuts down for any other reason.
 *
 * Configuration is hardcoded for the sample. A production gateway would
 * load configuration from a TOML file.
 *
 * Usage:
 *   SampleFixGateway gateway;
 *   gateway.run();
 */
class SampleFixGateway {
public:
    /**
     * @brief Constructs the gateway with hardcoded sample configuration.
     *
     * Creates the logger, reactor, and gateway thread. Registers the inbound
     * listener and the gateway thread with the reactor. Does not start the
     * reactor event loop -- call run() for that.
     */
    SampleFixGateway();

    /**
     * @brief Starts the reactor event loop.
     *
     * Blocks until the reactor shuts down (e.g. on SIGTERM or unrecoverable
     * error). Returns the reactor's exit code.
     *
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

private:
    // Configuration constants for the sample.
    static constexpr uint16_t    listen_port        = 9878;
    static constexpr int64_t     raw_buffer_capacity = 65536;
    static const     std::string listen_host;
    static const     std::string sender_comp_id;
    static const     std::string target_comp_id;
    static const     std::string logger_name;

    std::unique_ptr<pubsub_itc_fw::QuillLogger>   logger_;
    pubsub_itc_fw::ServiceRegistry                service_registry_;
    pubsub_itc_fw::ReactorConfiguration           reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor>        reactor_;
    std::shared_ptr<FixGatewayThread>              gateway_thread_;
};

} // namespace sample_fix_gateway
