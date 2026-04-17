#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include "FixMessage.hpp"
#include "FixParser.hpp"
#include "FixSerialiser.hpp"

namespace sample_fix_gateway {

/**
 * @brief ApplicationThread subclass that implements a minimal FIX 5.0SP2 gateway.
 *
 * FixGatewayThread receives raw FIX bytes from an inbound RawBytes connection,
 * feeds them to a FixParser, and handles:
 *
 *   - FIXT 1.1 session layer:
 *       Logon (A)        -- responds with Logon
 *       Heartbeat (0)    -- responds with Heartbeat
 *       TestRequest (1)  -- responds with Heartbeat carrying the TestReqID
 *       Logout (5)       -- responds with Logout and disconnects
 *
 *   - FIX 5.0SP2 application layer:
 *       NewOrderSingle (D) -- responds with a filled ExecutionReport (8)
 *
 * All other message types are logged and ignored.
 *
 * Session state:
 *   The thread tracks inbound and outbound sequence numbers. No persistent
 *   sequence number store is implemented -- sequence numbers reset to 1
 *   on each new connection, which is acceptable for a sample application.
 *
 * Threading:
 *   All methods are called from the ApplicationThread's own thread context.
 *   No external synchronisation is required.
 */
class FixGatewayThread : public pubsub_itc_fw::ApplicationThread {
public:
    /**
     * @brief Constructs a FixGatewayThread.
     *
     * @param[in] logger          Logger instance. Must outlive this object.
     * @param[in] reactor         The owning Reactor. Must outlive this object.
     * @param[in] sender_comp_id  SenderCompID for outbound FIX messages.
     * @param[in] target_comp_id  TargetCompID for outbound FIX messages.
     */
    FixGatewayThread(pubsub_itc_fw::QuillLogger& logger,
                     pubsub_itc_fw::Reactor& reactor,
                     std::string sender_comp_id,
                     std::string target_comp_id);

    /**
     * @brief Returns true if a FIX session is currently established.
     */
    [[nodiscard]] bool is_session_established() const {
        return session_established_.load(std::memory_order_acquire);
    }

protected:
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id,
                            const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

private:
    // Session message handlers
    void handle_logon(const FixMessage& msg);
    void handle_heartbeat(const FixMessage& msg);
    void handle_test_request(const FixMessage& msg);
    void handle_logout(const FixMessage& msg);

    // Application message handlers
    void handle_new_order_single(const FixMessage& msg);

    // Sends a FIX message on the current connection.
    void send_fix(FixMessage& msg);

    // Generates a unique order ID string.
    std::string generate_order_id();

    // Generates a unique execution ID string.
    std::string generate_exec_id();

    FixParser     parser_;
    FixSerialiser serialiser_;

    pubsub_itc_fw::ConnectionID conn_id_{};

    int outbound_seq_num_{1};
    int order_id_counter_{1};
    int exec_id_counter_{1};

    std::atomic<bool> session_established_{false};
};

} // namespace sample_fix_gateway
