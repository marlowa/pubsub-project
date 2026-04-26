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

#include "FixGatewaySeqConfiguration.hpp"
#include "FixMessage.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"

namespace sample_fix_gateway_seq {

/**
 * @brief ApplicationThread subclass for the sequencer-backed FIX gateway.
 *
 * Handles inbound FIX client connections (RawBytesProtocolHandler) and
 * outbound PDU connections to both sequencer instances.
 *
 * FIX session layer (per connection):
 *   Logon (A)        -- cancels logon timeout, responds with Logon
 *   Heartbeat (0)    -- responds with Heartbeat
 *   TestRequest (1)  -- responds with Heartbeat carrying TestReqID
 *   Logout (5)       -- responds with Logout and disconnects
 *
 * FIX application layer:
 *   NewOrderSingle (D)     -- encodes as fix_equity_orders PDU, sends to both
 *                             sequencers, records cl_ord_id -> session mapping
 *   OrderCancelRequest (F) -- encodes as fix_equity_orders PDU, sends to both
 *                             sequencers
 *
 * ExecutionReport PDUs arriving from the sequencer on the ER inbound listener
 * are decoded and routed back to the originating FIX client via cl_ord_id.
 * ERs with no cl_ord_id are logged and dropped.
 *
 * Threading: ThreadID 1.
 */
class FixGatewaySeqThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger instance. Must outlive this object.
     * @param[in] reactor  The owning Reactor. Must outlive this object.
     * @param[in] config   Gateway configuration.
     */
    FixGatewaySeqThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                        pubsub_itc_fw::QuillLogger& logger,
                        pubsub_itc_fw::Reactor& reactor,
                        const FixGatewaySeqConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // FIX session message handlers
    void handle_logon(FixSession& session, const FixMessage& msg);
    void handle_heartbeat(FixSession& session, const FixMessage& msg);
    void handle_test_request(FixSession& session, const FixMessage& msg);
    void handle_logout(FixSession& session, const FixMessage& msg);
    void handle_new_order_single(FixSession& session, const FixMessage& msg);
    void handle_order_cancel_request(FixSession& session, const FixMessage& msg);

    void disconnect_session(FixSession& session, const std::string& reason);
    void send_fix_to_session(FixSession& session, FixMessage& msg);

    // Forward a raw PDU buffer to both sequencer connections.
    void forward_to_sequencers(const void* data, uint32_t size);

    const FixGatewaySeqConfiguration& config_;

    // Active FIX client sessions keyed by ConnectionID.
    std::unordered_map<pubsub_itc_fw::ConnectionID, FixSession> sessions_;

    // Stateless serialiser shared across all sessions.
    FixSerialiser serialiser_;

    // ConnectionID of the primary sequencer outbound connection.
    pubsub_itc_fw::ConnectionID sequencer_primary_conn_id_;

    // ConnectionID of the secondary sequencer outbound connection.
    pubsub_itc_fw::ConnectionID sequencer_secondary_conn_id_;

    // cl_ord_id -> ConnectionID of the originating FIX client session.
    // Used to route ExecutionReport PDUs back to the correct FIX client.
    // ERs with no cl_ord_id are logged and dropped.
    std::unordered_map<std::string, pubsub_itc_fw::ConnectionID> cl_ord_id_to_session_;
};

} // namespace sample_fix_gateway_seq
