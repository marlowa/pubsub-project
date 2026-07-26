#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

#include <binary_session.hpp>
#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "BinaryGatewayConfiguration.hpp"
#include "BinarySession.hpp"
#include "GatewayIds.hpp"

namespace binary_gateway {

/**
 * @brief Application thread for the binary gateway.
 *
 * Does the same job as OrderGatewayThread without the translation. Its clients send
 * the very NewOrderSingle the pipeline carries, so an inbound order is wrapped in a
 * WalRecord envelope and forwarded as it arrived, and an inbound ExecutionReport is
 * relayed to the client without ever being decoded. There is no parser, no
 * serialiser and no dictionary here, because there is nothing to convert.
 *
 * Three kinds of connection arrive on this thread and are told apart by the service
 * name the reactor gives them:
 *
 *   - client connections, inbound on the client listener port;
 *   - the sequencer's ER connection, inbound on the ER listener port;
 *   - the gateway's own outbound connections to the sequencers.
 */
class BinaryGatewayThread : public pubsub_itc_fw::ApplicationThread {
  public:
    BinaryGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                        const BinaryGatewayConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    void handle_logon(BinarySession& session, const pubsub_itc_fw::EventMessage& message);
    void handle_new_order_single(BinarySession& session, const pubsub_itc_fw::EventMessage& message);
    void handle_order_cancel_request(BinarySession& session, const pubsub_itc_fw::EventMessage& message);
    void handle_execution_report(const pubsub_itc_fw::EventMessage& message);

    /** @brief Sends a LogonAck; on anything but Accepted the connection is then closed. */
    void send_logon_ack(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::LogonOutcome outcome, std::string_view text);

    /** @brief Returns the session for a client connection, or nullptr if there is none. */
    BinarySession* find_session(const pubsub_itc_fw::ConnectionID& id);

    /** @brief True when a comp id is already in use by another logged-on session. */
    [[nodiscard]] bool comp_id_in_use(std::string_view comp_id) const;

    /**
     * @brief Wraps an encoded order payload in a WalRecord envelope and forwards it.
     *
     * The order payload is forwarded exactly as the client sent it. Nothing here
     * decodes it: the routing metadata the sequencer needs travels on the envelope,
     * and the gateway has no reason to look inside a message it only carries. That
     * also means this gateway does not need rebuilding when an order message gains a
     * field.
     *
     * @param[in] inner_pdu_id The PDU id of the payload being wrapped.
     * @param[in] payload      Encoded order bytes, borrowed for the call.
     * @param[in] size         Length of @p payload in bytes.
     * @param[in] session      The originating client session.
     */
    void forward_order_in_envelope(int16_t inner_pdu_id, const uint8_t* payload, size_t size, const BinarySession& session);

    /** @brief Sends an already-encoded envelope PDU to both sequencers when HA is on. */
    void forward_envelope_to_sequencers(const pubsub_itc_fw_app::WalRecord& envelope);

    /**
     * @brief Records or retires an order in the session's open set from an ER.
     *
     * Tracking is driven by the matching engine's acknowledgements, not by order-forward
     * time: only an order the engine has accepted is actually on the book and therefore
     * the session's to cancel. A terminal status retires the entry.
     *
     * @param[in,out] session The session the report belongs to.
     * @param[in]     report  The decoded ExecutionReport.
     */
    void track_open_order(BinarySession& session, const pubsub_itc_fw_app::ExecutionReportView& report);

    /**
     * @brief Moves a departing session's resting orders onto the cancel-drain queue.
     *
     * A client that disappears leaves orders on the book that nobody is managing, so the
     * gateway cancels them on its behalf. The orders are moved out of the session (which
     * is about to be destroyed) and drained in batches by a timer rather than in one
     * burst, so that a client with thousands of resting orders cannot stall the reactor.
     *
     * @param[in,out] session The disconnecting session.
     */
    void queue_session_for_cleanup(BinarySession& session);

    /** @brief Timer callback: sends the next batch of OrderCancelRequests. */
    void drain_pending_cancels();

    const BinaryGatewayConfiguration& config_;

    // Client sessions keyed by connection id value. A connection is entered here when
    // it is accepted, before Logon, so that an order arriving too early can be refused
    // against a known session rather than an absent one.
    std::unordered_map<int32_t, BinarySession> sessions_;

    pubsub_itc_fw::ConnectionID sequencer_primary_conn_id_{};
    pubsub_itc_fw::ConnectionID sequencer_secondary_conn_id_{};

    // Service name the reactor gives connections accepted on the client listener, and
    // on the ER listener. Built once at construction; compared per connection.
    const std::string client_inbound_service_;
    const std::string er_inbound_service_;

    // Reusable buffer for encoding the WalRecord envelope. Grown to the largest order
    // seen (measure then fit) and reused, so no per-order allocation after warmup and
    // no fixed cap that could silently drop a large order.
    std::vector<uint8_t> envelope_encode_buffer_;

    // Storage for the open-order entries of every session, so tracking an order costs no
    // heap allocation. Created in on_app_ready_event, once the thread is running.
    std::unique_ptr<pubsub_itc_fw::ExpandablePoolAllocator<open_orders::OpenOrderEntry>> open_order_pool_;

    // A disconnected session's orders, held until their cancels have been sent. The
    // session itself is gone by then, so the comp id and connection id it needs are
    // copied here; next_order_index walks the vector as the drain proceeds.
    struct DeadSession {
        std::vector<open_orders::OpenOrderEntry*> open_orders;
        size_t next_order_index{0};
        int32_t session_conn_id{0};
        std::string comp_id;
        int cancel_id_counter{1};
    };

    std::deque<DeadSession> pending_cancel_sessions_;

    // True while the cancel-drain one-shot timer is armed, so it is not double-armed.
    bool cancel_drain_timer_active_{false};

    // Timer id of the cancel-drain one-shot, so on_timer_event can recognise it.
    pubsub_itc_fw::TimerID cancel_drain_timer_id_{};

    // Reusable buffer for encoding a generated OrderCancelRequest before it is wrapped
    // in an envelope. Grown to fit and reused, as the envelope buffer is.
    std::vector<uint8_t> cancel_encode_buffer_;
};

} // namespaces
