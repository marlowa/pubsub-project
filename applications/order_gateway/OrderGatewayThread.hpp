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

#include "OrderGatewayConfiguration.hpp"
#include "FixMessage.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"

// authentication.hpp must be included before fix_equity_orders.hpp because only
// authentication.hpp defines BytesView inside the PUBSUB_ITC_FW_APP_DSL_SHARED_HELPERS
// guard block; fix_equity_orders.hpp sets the guard without providing BytesView.
#include <authentication.hpp>

// DSL-generated message types for the equity order flow.
// The generated header lives in the build tree under dsl/.
#include <fix_equity_orders.hpp>

// Shared SCRAM-SHA-256 crypto primitives.
#include <scram_crypto/ScramCrypto.hpp>

namespace order_gateway {

/**
 * @brief ApplicationThread subclass for the sequencer-backed FIX gateway.
 *
 * Handles inbound FIX client connections (RawBytesProtocolHandler) and
 * an outbound PDU connection to the primary sequencer instance. A second
 * outbound to a follower sequencer will return when the leader-follower
 * protocol is implemented.
 *
 * FIX session layer (per connection):
 *   Logon (A)        -- cancels logon timeout, responds with Logon
 *   Heartbeat (0)    -- responds with Heartbeat
 *   TestRequest (1)  -- responds with Heartbeat carrying TestReqID
 *   Logout (5)       -- responds with Logout and disconnects
 *
 * FIX application layer:
 *   NewOrderSingle (D)     -- encodes as fix_equity_orders PDU, sends to the
 *                             primary sequencer, records cl_ord_id -> session
 *                             mapping
 *   OrderCancelRequest (F) -- encodes as fix_equity_orders PDU, sends to the
 *                             primary sequencer
 *
 * ExecutionReport PDUs arriving from the sequencer on the ER inbound listener
 * are decoded and routed back to the originating FIX client via cl_ord_id.
 * ERs with no cl_ord_id are logged and dropped.
 *
 * Threading: ThreadID 1.
 */
class OrderGatewayThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger instance. Must outlive this object.
     * @param[in] reactor  The owning Reactor. Must outlive this object.
     * @param[in] config   Gateway configuration.
     */
    OrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                        const OrderGatewayConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // Authentication PDU handlers (inbound from the authentication service).
    void handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message);
    void handle_authentication_result(const pubsub_itc_fw::EventMessage& message);

    // FIX session message handlers.
    // All inbound messages are received as ParsedFixMessage: string_views into
    // the MirroredBuffer, valid only for the duration of the callback.
    void handle_logon(FixSession& session, const ParsedFixMessage& msg);
    void handle_heartbeat(FixSession& session, const ParsedFixMessage& msg);
    void handle_test_request(FixSession& session, const ParsedFixMessage& msg);
    void handle_logout(FixSession& session, const ParsedFixMessage& msg);
    void handle_new_order_single(FixSession& session, const ParsedFixMessage& msg);
    void handle_order_cancel_request(FixSession& session, const ParsedFixMessage& msg);

    void disconnect_session(const FixSession& session, const std::string& reason);
    void send_fix_to_session(FixSession& session, const FixMessage& msg);

    /**
     * @brief Sends an ExecutionReport-Rejected back to the originating client
     *        when an inbound order/cancel cannot be forwarded (e.g. primary
     *        sequencer not connected).
     *
     * The reject is built locally by the gateway -- the matching engine never
     * sees the order -- so OrderID and ExecID are synthesised from
     * session.order_id_counter and session.exec_id_counter. The client gets a
     * structurally identical ExecutionReport (MsgType=8) so existing FIX
     * parsing handles it without special casing.
     *
     * Tag values follow FIX 5.0SP2:
     *   - 35 = 8           (ExecutionReport)
     *   - 150 = 8          (ExecType = Rejected)
     *   - 39  = 8          (OrdStatus = Rejected)
     *   - 103 = 99         (OrdRejReason = Other)
     *   - 58  = reason     (human-readable explanation)
     *
     * @param[in] session   The originating FIX client session. Its outbound
     *                      seq num, OrderID counter, and ExecID counter are
     *                      advanced as a side effect.
     * @param[in] inbound   The inbound NewOrderSingle or OrderCancelRequest
     *                      whose ClOrdID, Symbol, Side etc. are echoed back.
     *                      String_views in inbound are valid for this call only.
     * @param[in] reason    Text for tag 58.
     * @param[in] is_cancel True if rejecting an OrderCancelRequest (in which
     *                      case OrigClOrdID is echoed too); false for NOS.
     */
    void send_reject_execution_report(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason, bool is_cancel);

    // Forward a DSL PDU to the primary sequencer.
    // Template so it works with any DSL message type generated by the DSL tool.
    // The function name retains its plural form because dual-publish to a
    // follower will return when the leader-follower protocol is implemented;
    // until then there is only one target.
    template <typename MsgT> void forward_pdu_to_sequencers(int16_t pdu_id, const MsgT& msg) {
        if (sequencer_primary_conn_id_.get_value() != 0) {
            send_pdu(sequencer_primary_conn_id_, pdu_id, 0, msg);
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "OrderGatewayThread: primary sequencer not connected -- PDU not forwarded to primary");
        }
        if (config_.ha_enabled) {
            if (sequencer_secondary_conn_id_.get_value() != 0) {
                send_pdu(sequencer_secondary_conn_id_, pdu_id, 0, msg);
            } else {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                               "OrderGatewayThread: secondary sequencer not connected -- PDU not forwarded to secondary");
            }
        }
    }

    const OrderGatewayConfiguration& config_;

    // Precomputed inbound service name for the sequencer ER listener port.
    const std::string er_inbound_svc_;

    // Active FIX client sessions keyed by ConnectionID.
    std::unordered_map<pubsub_itc_fw::ConnectionID, FixSession> sessions_;

    // Stateless serialiser shared across all sessions.
    FixSerialiser serialiser_;

    // ConnectionID of the outbound connection to the primary authentication service.
    pubsub_itc_fw::ConnectionID auth_service_primary_conn_id_;

    // ConnectionID of the outbound connection to the secondary authentication service.
    // Only connected when ha_enabled is true.
    pubsub_itc_fw::ConnectionID auth_service_secondary_conn_id_;

    // ConnectionID of the primary sequencer outbound connection.
    pubsub_itc_fw::ConnectionID sequencer_primary_conn_id_;

    // ConnectionID of the secondary sequencer outbound connection.
    pubsub_itc_fw::ConnectionID sequencer_secondary_conn_id_;

    // gateway_session_conn_id → FixSession lookup: O(1) direct map lookup by
    // the internal connection ID stamped by the gateway on each NOS and echoed
    // back by the sequencer on each forwarded ER.  Returns nullptr if the
    // session has since disconnected.
    FixSession* find_session_by_conn_id(int32_t gateway_session_conn_id);

    // Legacy comp_id lookup retained for diagnostics; no longer used for ER routing.
    // Linear scan over sessions_ (small set; typically 1-10 sessions).
    // Returns nullptr if no matching session is found.
    FixSession* find_session_by_comp_id(const std::string& comp_id);
};

} // namespace order_gateway
