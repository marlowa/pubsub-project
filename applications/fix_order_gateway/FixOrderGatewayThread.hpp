#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
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
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include "CancelClOrdId.hpp"
#include "FixCapture.hpp"
#include "FixMessage.hpp"
#include "FixOrderGatewayConfiguration.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"
#include "GatewayIds.hpp"
#include "PoolMetricsReporter.hpp"

// authentication.hpp must be included before fix_orders.hpp because only
// authentication.hpp defines BytesView inside the PUBSUB_ITC_FW_APP_DSL_SHARED_HELPERS
// guard block; fix_orders.hpp sets the guard without providing BytesView.
#include <authentication.hpp>

// WalRecord doubles as the pipeline envelope carrying the routing metadata that
// must not live inside the (DD-derived) FIX PDU. Included after authentication.hpp
// so BytesView is already defined. See docs/fix/pdu_generation.md.
#include <leader_follower.hpp>

// ExecutionReportView appears in this header's own interface: send_execution_report_to_session
// is the one path both live and resent reports go through. Included after authentication.hpp
// for the BytesView reason above.
#include <fix_orders.hpp>

// Shared SCRAM-SHA-256 crypto primitives.
#include <scram_crypto/ScramCrypto.hpp> // IWYU pragma: keep

namespace fix_order_gateway {

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
class FixOrderGatewayThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger instance. Must outlive this object.
     * @param[in] reactor  The owning Reactor. Must outlive this object.
     * @param[in] config   Gateway configuration.
     */
    FixOrderGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                          const FixOrderGatewayConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
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

    /**
     * @brief Where an inbound message sits relative to what the venue expects from this member.
     *
     * The FIX session layer's whole answer to "may this message be processed". See
     * docs/fix/inbound_sequence_checking.md; classify_inbound_sequence has no side effects, so
     * both inbound paths can ask the same question and then do different things about it.
     */
    enum class InboundSequence {
        InSequence, ///< the number expected: process it, and the counter advances
        Duplicate,  ///< below expected but marked PossDupFlag: already processed, discard
        Gap,        ///< above expected: messages are missing, ask for them and process nothing
        TooLow,     ///< below expected and not marked: the far side has gone backwards
        Unnumbered, ///< no readable MsgSeqNum: nothing to place it by
    };
    static InboundSequence classify_inbound_sequence(const FixSession& session, const ParsedFixMessage& msg);

    /// Reads a message's MsgSeqNum, or 0 when it carries none that can be read.
    static int inbound_seq_num_of(const ParsedFixMessage& msg);

    /// Asks the member for everything from the expected number on, once per gap.
    void request_missing_messages(FixSession& session, int revealed_by_seq_num);

    /// Ends a session whose numbering the venue can no longer believe, telling the member why.
    void end_session_on_sequence_error(FixSession& session, int received_seq_num);

    /// What the Logon's own number turned out to be, judged once the venue knows what to expect.
    enum class LogonSequence { Proceed, ProceedThenAskForGap, EndSession };
    LogonSequence judge_logon_sequence(FixSession& session);

    /// Opens the session, in the order the specification requires when its Logon was out of
    /// sequence: judge, then reply, then ask -- or end the session without opening it.
    void establish_session_after_logon_sequence(FixSession& session);

    /// The session's report-number ranges not yet reported to the sequencer, ready for the wire.
    static std::vector<pubsub_itc_fw_app::SeqNumRange> unreported_report_seq_nums(const FixSession& session);

    /// Emits one SequenceReset-GapFill over the run of numbers, starting where the replay
    /// stands, that held something the venue cannot replay. See FixSession::report_seq_nums.
    void gap_fill_unreplayable_run(FixSession& session);

    // On client disconnect: moves the session's open_orders into the pending
    // cancel queue in O(1) and arms the drain timer.  The actual OCRs are
    // sent in batches by drain_pending_cancels() so the reactor thread is
    // never monopolised by a single disconnect with many open orders.
    // Handles a session's resting orders when its connection goes away.
    //
    // Persistent orders (GoodTillCancel, GoodTillDate) are never cancelled here: they were
    // placed to outlive the session, so they are released from the gateway's bookkeeping
    // and left resting on the book. Everything else is either queued for immediate
    // cancellation -- a clean Logout, or a zero grace period -- or parked in
    // grace_sessions_ until its deadline.
    void queue_session_for_cleanup(FixSession& session);

    // Timer callback: sends up to cancel_drain_batch_size OCRs from the
    // pending queue, then reschedules itself if more remain.
    void drain_pending_cancels();

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
                           "FixOrderGatewayThread: primary sequencer not connected -- PDU not forwarded to primary");
        }
        if (config_.ha_enabled) {
            if (sequencer_secondary_conn_id_.get_value() != 0) {
                send_pdu(sequencer_secondary_conn_id_, pdu_id, 0, msg);
            } else {
                PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                               "FixOrderGatewayThread: secondary sequencer not connected -- PDU not forwarded to secondary");
            }
        }
    }

    // Wrap an order PDU in a WalRecord envelope and forward it to the sequencer(s).
    // The routing metadata that must not live inside the (DD-derived) FIX PDU --
    // the originating session's connection id (for ER routing) and its SenderCompID
    // (for audit) -- rides on the envelope, not the PDU. The sequencer stamps seq_no
    // and wall_time_ns; both are left zero here. See docs/fix/pdu_generation.md.
    // @param[in] gateway_ingress_ns When the originating bytes were read off the client
    //            socket, or 0 for an order this gateway generated itself (the cancels
    //            issued on client disconnect). Zero is stamped as absent, so a
    //            gateway-invented order never contributes a round trip that no client
    //            ever waited for.
    template <typename MsgT>
    void forward_order_in_envelope(int16_t inner_pdu_id, const MsgT& msg, int32_t gateway_session_conn_id, std::string_view sender_comp_id,
                                   int64_t gateway_ingress_ns) {
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
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "FixOrderGatewayThread: failed to encode order PDU {} ({} bytes needed) -- not forwarded", inner_pdu_id, bytes_needed);
            return;
        }

        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.pdu_id = inner_pdu_id;
        envelope.payload.data = order_encode_buffer_.data();
        envelope.payload.size = bytes_written;
        envelope.has_gateway_session_conn_id = true;
        envelope.gateway_session_conn_id = gateway_session_conn_id;
        // Which gateway this came from, so the sequencer can send the ER back here rather
        // than to the binary gateway, whose session connection ids are numbered separately
        // and would otherwise be indistinguishable from these.
        envelope.has_origin_gateway_id = true;
        envelope.origin_gateway_id = gateway_ids::fix_order_gateway;
        // Which instance of this protocol. The protocol id alone stopped identifying a
        // process once a protocol could run more than one.
        envelope.has_gateway_instance_id = true;
        envelope.gateway_instance_id = config_.instance_id;
        // When this order was read off the client socket. The sequencer remembers it against
        // the order's seq_no and returns it on the acknowledging ER, which is what lets this
        // gateway measure the whole round trip without keeping any per-order state itself.
        envelope.has_gateway_ingress_ns = (gateway_ingress_ns != 0);
        envelope.gateway_ingress_ns = gateway_ingress_ns;
        if (!sender_comp_id.empty()) {
            envelope.has_sender_comp_id = true;
            envelope.sender_comp_id = sender_comp_id;
        }

        forward_pdu_to_sequencers(pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope);
    }

    const FixOrderGatewayConfiguration& config_;

    // Reusable scratch buffer for encoding an order PDU before wrapping it in a
    // WalRecord envelope. Grown to the largest order seen (measure then fit in
    // forward_order_in_envelope) and reused, so no per-order heap allocation after
    // warmup and no fixed cap that could silently drop an over-large order.
    std::vector<uint8_t> order_encode_buffer_;

    // Reusable, growable buffer for the outbound FIX ExecutionReport wire bytes. Sized
    // to execution_report_initial_buffer_size at construction and grown (grow-and-retry
    // in the ER send path) to the largest ER seen -- no fixed cap that could silently
    // drop an ER with many echoed group instances, no per-ER allocation after warmup.
    std::vector<char> er_wire_buffer_;

    // Reusable arena backing the extracted NOS repeating-group element arrays. Sized to
    // need (grow-and-retry in handle_new_order_single), not a fixed cap that could
    // silently drop groups from a large order. Grows to the high-water mark and is reused.
    static constexpr size_t initial_group_arena_size = 4096;
    static constexpr size_t max_group_arena_size = 1u << 20; // 1 MiB sanity ceiling
    std::vector<uint8_t> group_arena_buffer_;

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

    // Tell the sequencer where this session's execution reports should go, and that they
    // should stop coming here when it ends. The sequencer keys its routing on the session
    // identity and treats this connection as a destination it can replace, which is what
    // lets a member reconnect -- here or at its backup instance -- and still be sent
    // reports for orders it placed before. See docs/availability/gateway_ha.md.
    void announce_session_bound(const FixSession& session);
    void announce_session_unbound(const FixSession& session);

    // Re-announce, down a sequencer link that has just come up, every session still waiting
    // for its numbering. A member that logs on while the gateway is between connect retries
    // has its SessionBound dropped for want of anywhere to send it, and nothing else would
    // ever ask again -- the session then hangs until `sequence_state_timeout` opens it on
    // numbering the venue never confirmed. See docs/bug_list.md, BUG-0019.
    //
    // Sent down the one link named, NOT to both sequencers: a SessionBound that arrives at a
    // leader which already holds this binding is read as the previous session having died,
    // which deliberately raises the resume figure. That is the right answer for a real
    // failover and the wrong one for a retry, so a link that already took the announcement
    // must not be sent it twice.
    void retry_pending_session_binds(pubsub_itc_fw::ConnectionID sequencer_conn_id);

    // The venue's reply to a binding: what it remembers of this session's sequence numbers,
    // so the member's numbering continues across a reconnect instead of restarting at 1.
    void handle_session_bound_ack(const pubsub_itc_fw::EventMessage& message);

    /// Sends the FIX Logon reply and opens the session, once its numbering is settled.
    void complete_session_establishment(FixSession& session);

    // A session's missed execution reports, replayed from the sequencer's WAL in answer to a
    // ResendRequest, and the completion that ends the replay. See handle_resend_request.
    void handle_session_replay_record(const pubsub_itc_fw::EventMessage& message);
    void handle_session_replay_complete(const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief Encodes one ExecutionReport and sends it to a session. False if it would not fit.
     *
     * The single path for both live and resent reports, so the two cannot drift apart: a
     * resend differs only in PossDupFlag and OrigSendingTime, and if anything else about the
     * encoding differed the member would be told something subtly different about an event
     * it has already seen.
     *
     * @param[in] poss_dup              True when resending; adds PossDupFlag=Y.
     * @param[in] orig_sending_time_ns  When the venue first sent it; only read when resending.
     */
    bool send_execution_report_to_session(FixSession& session, const pubsub_itc_fw_app::ExecutionReportView& view, bool poss_dup, int64_t orig_sending_time_ns);

    // Sessions are keyed by connection, but a replay is addressed to neither: the reply comes
    // back from the sequencer naming the request, and a bind ack names the comp id. Linear
    // scans because a gateway holds tens of sessions, not thousands, and both are logon-time
    // paths -- an index here would be state to keep correct for no measurable gain.
    FixSession* find_session_by_comp_id(std::string_view comp_id);
    FixSession* find_session_by_replay_request(int64_t request_id);

    // Correlates a replay request with the records that answer it. Monotonic per process;
    // the sequencer treats it as opaque and echoes it back.
    int64_t next_replay_request_id_{0};

    // Reports resent to members that asked for messages they missed. Counted separately from
    // execution_reports_sent_ because a resend is not new venue activity: folding the two
    // together would make a recovering session look like a burst of trading.
    int64_t execution_reports_replayed_{0};

    // Legacy comp_id lookup retained for diagnostics; no longer used for ER routing.
    // Linear scan over sessions_ (small set; typically 1-10 sessions).
    // Returns nullptr if no matching session is found.
    FixSession* find_session_by_comp_id(const std::string& comp_id);

    // Holds the open-orders state of a disconnected session, pending deferred
    // cancellation.
    //
    // A flat vector rather than the session's OpenOrderMap. Once a session is dead
    // its orders are only ever consumed in sequence -- nothing looks one up by
    // ClOrdID again -- so the hash map earns nothing here and costs a great deal:
    // draining it meant calling begin() per order, and robin_map::begin() scans its
    // bucket array from the start, rescanning every bucket already emptied. That made
    // draining N orders O(N^2) in bucket probes and put this path at 11% of the
    // gateway profile. next_order_index walks the vector instead, so each order is
    // O(1) and the batching across timer ticks still resumes where it left off.
    struct DeadSession {
        std::vector<OpenOrderEntry*> open_orders;
        size_t next_order_index{0};
        int32_t session_conn_id{0};
        std::string client_comp_id;
        int cancel_id_counter{1};
        // When this session's orders become due for cancellation. Only meaningful while
        // the session sits in grace_sessions_; the drain queue ignores it.
        std::chrono::steady_clock::time_point cancel_due{};
    };

    // Sessions waiting for their open orders to be cancelled.  Entries are appended
    // at disconnect time and consumed by the drain timer.
    std::deque<DeadSession> pending_cancel_sessions_;

    // Sessions whose connection dropped and whose grace period has not yet expired.
    // Ordered by cancel_due, which is automatic: every entry takes the same grace period
    // and they are appended in the order their connections died, so the front is always
    // the earliest due.
    //
    // A session leaves here one of two ways: the grace timer expires and it moves to
    // pending_cancel_sessions_, or the same comp id logs on again and it is discarded
    // without a single cancel being sent -- which is the entire point of the feature.
    std::deque<DeadSession> grace_sessions_;

    // One-off timer set for grace_sessions_.front().cancel_due. Rearmed on each
    // expiry while entries remain.
    pubsub_itc_fw::TimerID grace_timer_id_{};

    // Drives the periodic report of each session's outbound sequence number to the sequencer.
    //
    // Periodic rather than only-at-unbind because the value matters most exactly when it
    // cannot be sent: a gateway killed outright reports nothing, and the sequencer then has
    // no idea where the session had reached. It started the returning member at 1, which --
    // with a client whose own store had restarted too -- looked like a clean new session
    // while thousands of that member's orders were live on the book.
    pubsub_itc_fw::TimerID sequence_report_timer_id_{};

    /** @brief Tells the sequencer where each established session's numbering has reached. */
    void report_session_sequence_numbers();
    bool grace_timer_active_{false};

    // Moves every grace_sessions_ entry whose deadline has passed into the cancel queue
    // and rearms for the next, if any.
    void expire_grace_sessions();

    // Arms (or rearms) the grace timer for the front entry's deadline.
    void arm_grace_timer();

    // Discards any grace-period entry belonging to this comp id, cancelling nothing.
    // Called when a comp id logs on: if it got back inside its window, its orders are
    // left resting exactly as they were.
    //
    // Returns the number of orders reclaimed, for logging.
    size_t reclaim_grace_session(std::string_view comp_id);

    // When the current raw-socket read event was handled, in wall-clock nanoseconds.
    // Stamped on every order forwarded out of that event and returned on the acknowledging
    // ER, where it becomes the start of the round-trip measurement. Zero until the first
    // read event, which only a gateway-generated order can precede.
    int64_t current_read_ingress_ns_{0};

    // Nanoseconds from reading an order off the client socket to starting to send its
    // ExecutionReport. Unbound -- and therefore a no-op -- unless this thread was given a
    // metrics scope. See applications/fix_common/GatewayMetrics.hpp for why the bucket
    // bounds are shared with the binary gateway rather than chosen here.
    pubsub_itc_fw::HistogramHandle order_round_trip_histogram_;

    // Publishes the open-order pool's statistics. Deliberately identical to the binary
    // gateway's -- same metric family, same scope, same sample interval -- since a
    // difference in any of them would make a comparison between the two protocols measure
    // the instrumentation. See applications/fix_common/GatewayMetrics.hpp.
    fix_common::PoolMetricsReporter open_order_pool_metrics_;
    pubsub_itc_fw::TimerID pool_metrics_timer_id_{};

    // Running totals behind the GW-PROGRESS line. Per-order logging was moved to Debug
    // because it cost roughly a third of this gateway's CPU under load; these keep the
    // counts available at a thousandth of that.
    int64_t orders_received_{0};
    int64_t execution_reports_sent_{0};
    int64_t execution_reports_dropped_{0};

    // Cancels sent since this drain began, so the completion line can report the whole
    // drain rather than whatever the final tick happened to do.
    int64_t cancels_sent_this_drain_{0};

    // True while the cancel-drain one-shot timer is armed.  Prevents double-arming.
    bool cancel_drain_timer_active_{false};

    // Timer id of the cancel-drain one-shot, so on_timer_event can recognise it.
    pubsub_itc_fw::TimerID cancel_drain_timer_id_{};
};

} // namespaces
