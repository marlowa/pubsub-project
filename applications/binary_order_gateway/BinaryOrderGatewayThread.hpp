#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
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
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

// authentication.hpp must be included before the others because only it defines BytesView
// inside the PUBSUB_ITC_FW_APP_DSL_SHARED_HELPERS guard block.
#include <authentication.hpp>

#include <binary_session.hpp>
#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include <scram_crypto/ScramCrypto.hpp> // IWYU pragma: keep

#include "BinaryOrderGatewayConfiguration.hpp"
#include "BinarySession.hpp"
#include "CancelClOrdId.hpp"
#include "GatewayIds.hpp"

namespace binary_order_gateway {

/**
 * @brief Application thread for the binary gateway.
 *
 * Does the same job as FixOrderGatewayThread without the translation. Its clients send
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
class BinaryOrderGatewayThread : public pubsub_itc_fw::ApplicationThread {
  public:
    BinaryOrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                             const BinaryOrderGatewayConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    void handle_logon(BinarySession& session, const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief Starts the SCRAM-SHA-256 exchange for a session that has sent a valid Logon.
     *
     * The same exchange the FIX gateway runs, against the same service: a fresh client
     * nonce goes out with the comp id, and the session waits. The password is not sent --
     * only a proof derived from it, once the challenge comes back.
     */
    void begin_scram_authentication(BinarySession& session);

    void handle_authentication_challenge(const pubsub_itc_fw::EventMessage& message);
    void handle_authentication_result(const pubsub_itc_fw::EventMessage& message);

    /** @brief Refuses a logon, sends the reason, and closes the connection. */
    void refuse_logon(BinarySession& session, pubsub_itc_fw_app::LogonOutcome outcome, std::string_view text);

    /** @brief Returns the session with the given connection id value, or nullptr. */
    BinarySession* find_session_by_conn_id(int32_t conn_id_value);
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

    /**
     * @brief Logs a running total of accounted-for execution reports.
     *
     * **CONTRACT WITH THE PERFORMANCE HARNESS -- DO NOT CHANGE THE MARKER OR ITS FIELDS
     * WITHOUT CHANGING perf_run.py AND callgrind_run.py TO MATCH.**
     *
     * `perf_run.py` parses the `GW-PROGRESS accounted=N` line to decide when a run has
     * finished: it is the authoritative end-to-end signal that every order has completed the
     * NOS -> ME -> sequencer -> gateway -> client round trip. Renaming the marker, dropping a
     * field, changing the cadence, or raising the level above Info does not fail any unit test
     * -- the run simply hangs until it times out, and the harness reports a stall that looks
     * like a pipeline fault rather than a logging change.
     *
     * The per-order GW-NOS-RECV and GW-ER-SENT lines this replaced are at Debug because at
     * 200,000 orders they cost around a third of the gateway's CPU in Quill alone -- more than
     * the entire FIX parse -- which made any gateway-to-gateway comparison measure logging
     * rather than protocol. Both gateways emit this same marker at the same cadence for that
     * reason; keep them symmetrical.
     */
    void report_order_progress();

    /** @brief Timer callback: sends the next batch of OrderCancelRequests. */
    void drain_pending_cancels();

    const BinaryOrderGatewayConfiguration& config_;

    // Client sessions keyed by connection id value. A connection is entered here when
    // it is accepted, before Logon, so that an order arriving too early can be refused
    // against a known session rather than an absent one.
    std::unordered_map<int32_t, BinarySession> sessions_;

    pubsub_itc_fw::ConnectionID auth_service_primary_conn_id_{};
    pubsub_itc_fw::ConnectionID auth_service_secondary_conn_id_{};
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
        // Only meaningful while the entry sits in grace_sessions_.
        std::chrono::steady_clock::time_point cancel_due{};
    };

    std::deque<DeadSession> pending_cancel_sessions_;

    // Sessions whose connection dropped and whose grace period has not yet expired. Same
    // contract as the FIX order gateway's: an entry leaves either because its deadline
    // passed, or because the same comp id logged on again -- in which case nothing is
    // cancelled at all. Ordered by deadline, which is automatic since every entry takes
    // the same grace period and they are appended as connections die.
    std::deque<DeadSession> grace_sessions_;

    pubsub_itc_fw::TimerID grace_timer_id_{};
    bool grace_timer_active_{false};

    void expire_grace_sessions();
    void arm_grace_timer();
    size_t reclaim_grace_session(std::string_view comp_id);

    // When the current client PDU was taken off the connection, in wall-clock nanoseconds.
    // Stamped on the order forwarded out of it and returned on the acknowledging ER, where
    // it becomes the start of the round-trip measurement. Zero until the first client PDU,
    // which only a gateway-generated cancel can precede.
    int64_t current_pdu_ingress_ns_{0};

    // Nanoseconds from taking an order off the client connection to starting to send its
    // ExecutionReport. The same metric name and bucket bounds the FIX gateway registers,
    // told apart by the component label -- see applications/fix_common/GatewayMetrics.hpp.
    // Unbound, and therefore a no-op, when no bounds are configured.
    pubsub_itc_fw::HistogramHandle order_round_trip_histogram_;

    // Running totals behind the GW-PROGRESS line, matching the FIX order gateway's so the two
    // gateways log the same amount and a perf comparison is not measuring logging.
    int64_t orders_received_{0};
    int64_t execution_reports_sent_{0};
    int64_t execution_reports_dropped_{0};

    // Cancels sent since this drain began, so the completion line can report the whole
    // drain rather than whatever the final tick happened to do.
    int64_t cancels_sent_this_drain_{0};

    // True while the cancel-drain one-shot timer is armed, so it is not double-armed.
    bool cancel_drain_timer_active_{false};

    // Timer id of the cancel-drain one-shot, so on_timer_event can recognise it.
    pubsub_itc_fw::TimerID cancel_drain_timer_id_{};

    // Reusable buffer for encoding a generated OrderCancelRequest before it is wrapped
    // in an envelope. Grown to fit and reused, as the envelope buffer is.
    std::vector<uint8_t> cancel_encode_buffer_;
};

} // namespaces
