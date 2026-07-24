#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include "FixCapture.hpp"
#include "FixMessage.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"
#include "OrderGatewayConfiguration.hpp"

// authentication.hpp must be included before fix_orders.hpp because only
// authentication.hpp defines BytesView inside the PUBSUB_ITC_FW_APP_DSL_SHARED_HELPERS
// guard block; fix_orders.hpp sets the guard without providing BytesView.
#include <authentication.hpp>

// WalRecord doubles as the pipeline envelope carrying the routing metadata that
// must not live inside the (DD-derived) FIX PDU. Included after authentication.hpp
// so BytesView is already defined. See docs/design/fix_pdu_generation.md.
#include <leader_follower.hpp>

// Shared SCRAM-SHA-256 crypto primitives.
#include <scram_crypto/ScramCrypto.hpp> // IWYU pragma: keep

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
 *   NewOrderSingle (D)     -- encodes as fix_orders PDU, sends to the
 *                             primary sequencer, records cl_ord_id -> session
 *                             mapping
 *   OrderCancelRequest (F) -- encodes as fix_orders PDU, sends to the
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
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
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
    void handle_resend_request(FixSession& session, const ParsedFixMessage& msg);
    void handle_new_order_single(FixSession& session, const ParsedFixMessage& msg, const fix_codec::FixMessageReader& reader);
    void handle_order_cancel_request(FixSession& session, const ParsedFixMessage& msg);

    void disconnect_session(const FixSession& session, const std::string& reason);
    void send_fix_to_session(FixSession& session, const FixMessage& msg);

    // On client disconnect: moves the session's open_orders into the pending
    // cancel queue in O(1) and arms the drain timer.  The actual OCRs are
    // sent in batches by drain_pending_cancels() so the reactor thread is
    // never monopolised by a single disconnect with many open orders.
    void queue_session_for_cleanup(FixSession& session);

    // Timer callback: sends up to cancel_drain_batch_size OCRs from the
    // pending queue, then reschedules itself if more remain.
    void drain_pending_cancels();

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

    void send_business_reject(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason);

    // Sends a FIX Reject (35=3) for a well-framed inbound message that failed
    // dictionary validation, carrying the session-reject reason and the specific
    // offending tag from the FixReject. Session-level; used only once the FIX
    // session is established (an invalid Logon is disconnected instead).
    void send_fix_reject(FixSession& session, const ParsedFixMessage& inbound, const fix_codec::FixReject& reject);

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

    // Wrap an order PDU in a WalRecord envelope and forward it to the sequencer(s).
    // The routing metadata that must not live inside the (DD-derived) FIX PDU --
    // the originating session's connection id (for ER routing) and its SenderCompID
    // (for audit) -- rides on the envelope, not the PDU. The sequencer stamps seq_no
    // and wall_time_ns; both are left zero here. See docs/design/fix_pdu_generation.md.
    template <typename MsgT>
    void forward_order_in_envelope(int16_t inner_pdu_id, const MsgT& msg, int32_t gateway_session_conn_id, std::string_view sender_comp_id) {
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        // Measure then fit: a zero-size out buffer makes encode report bytes_needed
        // without writing, then the reusable buffer is grown to hold it. This avoids a
        // fixed-size cap that would silently drop an over-large order. The buffer grows
        // once to the high-water mark and is reused, so there is no per-order allocation
        // after warmup. Unqualified encode so ADL finds pubsub_itc_fw_app::encode for the
        // concrete MsgT (the overload is not visible at this template's definition point).
        [[maybe_unused]] const bool measured = encode(msg, nullptr, 0, bytes_written, bytes_needed);
        if (order_encode_buffer_.size() < bytes_needed) {
            order_encode_buffer_.resize(bytes_needed);
        }
        if (!encode(msg, order_encode_buffer_.data(), order_encode_buffer_.size(), bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "OrderGatewayThread: failed to encode order PDU {} ({} bytes needed) -- not forwarded",
                       inner_pdu_id, bytes_needed);
            return;
        }

        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.pdu_id = inner_pdu_id;
        envelope.payload.data = order_encode_buffer_.data();
        envelope.payload.size = bytes_written;
        envelope.has_gateway_session_conn_id = true;
        envelope.gateway_session_conn_id = gateway_session_conn_id;
        if (!sender_comp_id.empty()) {
            envelope.has_sender_comp_id = true;
            envelope.sender_comp_id = sender_comp_id;
        }

        forward_pdu_to_sequencers(pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope);
    }

    const OrderGatewayConfiguration& config_;

    // Reusable scratch buffer for encoding an order PDU before wrapping it in a
    // WalRecord envelope. Grown to the largest order seen (measure then fit in
    // forward_order_in_envelope) and reused, so no per-order heap allocation after
    // warmup and no fixed cap that could silently drop an over-large order.
    std::vector<uint8_t> order_encode_buffer_;

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

    // FIX capture: non-null only when fix_capture_enabled=true in config.
    std::unique_ptr<FixCapture> capture_;

    // Pool for open-order string storage. Allocated in on_initial_event once
    // configuration is available. Owned for the lifetime of this thread.
    std::unique_ptr<pubsub_itc_fw::ExpandablePoolAllocator<OpenOrderEntry>> open_order_pool_;

    // gateway_session_conn_id -> FixSession lookup: O(1) direct map lookup by
    // the internal connection ID stamped by the gateway on each NOS and echoed
    // back by the sequencer on each forwarded ER.  Returns nullptr if the
    // session has since disconnected.
    FixSession* find_session_by_conn_id(int32_t gateway_session_conn_id);

    // Legacy comp_id lookup retained for diagnostics; no longer used for ER routing.
    // Linear scan over sessions_ (small set; typically 1-10 sessions).
    // Returns nullptr if no matching session is found.
    FixSession* find_session_by_comp_id(const std::string& comp_id);

    // Holds the open-orders state of a disconnected session, pending deferred
    // cancellation.  The map is moved from the FixSession in O(1) at disconnect
    // time; drain_pending_cancels() iterates it in small batches.
    struct DeadSession {
        OpenOrderMap open_orders;
        int32_t session_conn_id{0};
        std::string client_comp_id;
        int cancel_id_counter{1};
    };

    // Sessions waiting for their open orders to be cancelled.  Entries are
    // appended at disconnect time (O(1) map move) and consumed by the drain timer.
    std::deque<DeadSession> pending_cancel_sessions_;

    // True while the cancel-drain one-shot timer is armed.  Prevents double-arming.
    bool cancel_drain_timer_active_{false};
};

} // namespaces
