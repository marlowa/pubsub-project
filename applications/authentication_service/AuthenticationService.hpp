#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

#include "AuthenticationServiceConfiguration.hpp"
#include "AuthenticationThread.hpp"

namespace authentication_service {

/**
 * @brief Top-level application class for the authentication service.
 *
 * Owns all framework objects and wires them together:
 *
 *   - One inbound FrameworkPdu listener for gateway connections (plain TCP;
 *     this path is internal to the deployment and does not use TLS).
 *   - One AuthenticationThread that handles the SCRAM-SHA-256 protocol.
 *
 * Gateways connect as PDU clients and exchange SCRAM messages using the
 * four-PDU protocol defined in authentication.dsl.
 *
 * The logger is constructed in main() before the config is loaded.
 */
class AuthenticationService {
  public:
    /**
     * @brief Constructs the service and wires all connections.
     * @param[in] config Service configuration.
     * @param[in] logger Logger. Ownership transferred. Must already have the
     *                   correct log levels applied from config.
     */
    explicit AuthenticationService(const AuthenticationServiceConfiguration& config,
                                   std::unique_ptr<pubsub_itc_fw::QuillLogger> logger);

    /**
     * @brief Starts the reactor event loop. Blocks until shutdown.
     * @return 0 on normal shutdown, non-zero on error.
     */
    int run();

  private:
    AuthenticationServiceConfiguration config_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger_;
    pubsub_itc_fw::ServiceRegistry service_registry_;
    pubsub_itc_fw::ReactorConfiguration reactor_configuration_;
    std::unique_ptr<pubsub_itc_fw::Reactor> reactor_;
    std::shared_ptr<AuthenticationThread> authentication_thread_;
};

} // namespace authentication_service
