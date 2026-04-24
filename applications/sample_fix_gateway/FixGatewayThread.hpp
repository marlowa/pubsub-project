#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include "FixGatewayConfiguration.hpp"
#include "FixMessage.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"

namespace sample_fix_gateway {

/**
 * @brief ApplicationThread subclass that implements a minimal FIX 5.0SP2 gateway.
 *
 * FixGatewayThread accepts multiple concurrent FIX client connections. Each
 * connection has its own independent FixSession which owns the per-connection
 * parser state, sequence numbers, and session establishment flag.
 *
 * Connection lifecycle:
 *   on_connection_established() -- creates a FixSession, starts logon timeout timer
 *   on_raw_socket_message()     -- verifies FIX preamble, feeds bytes to session parser,
 *                                  commits consumed bytes using message.connection_id()
 *   on_timer_event()            -- fires logon timeout if client fails to Logon in time
 *   on_connection_lost()        -- cancels logon timeout timer, removes session
 *
 * FIX session layer handled per connection:
 *   Logon (A)        -- cancels logon timeout, responds with Logon
 *   Heartbeat (0)    -- responds with Heartbeat
 *   TestRequest (1)  -- responds with Heartbeat carrying the TestReqID
 *   Logout (5)       -- responds with Logout and disconnects
 *
 * FIX application layer:
 *   NewOrderSingle (D) -- responds with a filled ExecutionReport (8)
 *
 * The FixSerialiser is stateless and shared across all sessions.
 */
class FixGatewayThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @brief Constructs a FixGatewayThread.
     * @param[in] token     Constructor token to force use of factory
     * @param[in] logger    Logger instance. Must outlive this object.
     * @param[in] reactor   The owning Reactor. Must outlive this object.
     * @param[in] config    Gateway configuration.
     */
    FixGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                     const FixGatewayConfiguration& config);

    /**
     * @brief Returns the number of currently active FIX sessions.
     */
    [[nodiscard]] int session_count() const {
        return static_cast<int>(sessions_.size());
    }

  protected:
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // FIX session message handlers
    void handle_logon(FixSession& session, const FixMessage& msg);
    void handle_heartbeat(FixSession& session, const FixMessage& msg);
    void handle_test_request(FixSession& session, const FixMessage& msg);
    void handle_logout(FixSession& session, const FixMessage& msg);
    void handle_new_order_single(FixSession& session, const FixMessage& msg);

    // Disconnect a session cleanly -- cancels its timer and tears down the connection.
    void disconnect_session(FixSession& session, const std::string& reason);

    void send_fix_to_session(FixSession& session, FixMessage& msg);

    static std::string generate_order_id(FixSession& session);
    static std::string generate_exec_id(FixSession& session);

    const FixGatewayConfiguration& config_;

    // Active sessions keyed by ConnectionID.
    std::unordered_map<pubsub_itc_fw::ConnectionID, FixSession> sessions_;

    // Shared across all sessions -- FixSerialiser is stateless.
    FixSerialiser serialiser_;
};

} // namespace sample_fix_gateway
