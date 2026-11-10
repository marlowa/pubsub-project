#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/UnansweredCommands.hpp>

#include <fix_order_gateway_test_events.hpp>
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <pubsub_itc_fw/ThreadConfinementCheck.hpp>
#endif

#include "FixCapture.hpp"
#include "FixMessage.hpp"
#include "FixOrderGatewayConfiguration.hpp"
#include "FixRejections.hpp"
#include "FixSerialiser.hpp"
#include "FixSession.hpp"
#include "GatewayIds.hpp"
#include "GatewayProgressMetrics.hpp"
#include "GatewayRefusal.hpp"
#include "RefusedCommand.hpp"
#include "RequestRefusal.hpp"
#include "SentAgainEnvelope.hpp"
#include "ThrottleRefusalMetrics.hpp"
#include "ThrottledCommand.hpp"

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

    /// Sends one ResendRequest and arms the timer that bounds the wait for an answer.
    void send_resend_request(FixSession& session);

    /// On that timer: asks again, or ends a session whose member is not answering.
    void retry_or_abandon_resend_request(FixSession& session);

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

    // A session's connection has gone. What the venue will do with the member's orders is said, at Info and as a test
    // event. The gateway cancels nothing itself: the leading sequencer carries
    // out the member's cancel-on-disconnect instruction where the orders are, which it can do after this gateway has
    // restarted or died too (docs/availability/cancel_on_disconnect_where_the_orders_are.md). The session is kept in
    // grace_sessions_ for its grace period, so that the gateway can say whether the member came back in time, and stop
    // keeping the session's unanswered commands once it has not.
    void note_session_ended(FixSession& session);

    /**
     * @brief Logs a running total of accounted-for execution reports.
     *
     * The `GW-PROGRESS` line tells a person reading the Quill log how far a run has got: how many
     * orders arrived, how many were answered, and how many await an answer. Programs do not read
     * it. The same totals are published as counters (fix_common::GatewayProgressMetrics), which
     * perf_run.py and callgrind_run.py read to decide when a run has finished.
     *
     * The per-order GW-NOS-RECV and GW-ER-SENT lines this replaced are at Debug because at
     * 200,000 orders they cost around a third of the gateway's CPU in Quill alone -- more than
     * the entire FIX parse -- which made any gateway-to-gateway comparison measure logging
     * rather than protocol. Both gateways emit this same marker at the same cadence for that
     * reason; keep them symmetrical.
     */
    void report_order_progress();

    /// Emits the health line on a timer when the count-driven one has fallen silent.
    void report_order_progress_on_timer();

    /// Writes the line itself, from either path.
    void emit_order_progress(int64_t accounted);

    /**
     * @brief Sends an ExecutionReport-Rejected back to the originating client
     *        when an inbound new order cannot be forwarded (e.g. primary
     *        sequencer not connected).
     *
     * For a NewOrderSingle only. A refused request to cancel is answered with
     * refuse_cancel instead, because a rejected ExecutionReport says the order was
     * rejected, and the order a refused cancel names is still open.
     *
     * The reject is built locally by the gateway -- the matching engine never
     * sees the order -- so OrderID and ExecID are synthesised from
     * session.order_id_counter and session.exec_id_counter.
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
     * @param[in] inbound   The inbound NewOrderSingle whose ClOrdID, Symbol, Side
     *                      etc. are echoed back. String_views in inbound are valid
     *                      for this call only.
     * @param[in] reason    Text for tag 58.
     */
    void send_reject_execution_report(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason,
                                      pubsub_itc_fw_app::OrdRejReason ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other);

    /**
     * @brief The same reject ExecutionReport, built from the order's fields rather than from a FIX message the member
     *        sent, for an order refused after it was sent to the sequencers.
     * @param[in,out] session The member's session. Its outbound seq num, OrderID counter and ExecID counter are advanced.
     * @param[in] cl_ord_id The order's ClOrdID, echoed back.
     * @param[in] symbol The order's Symbol, echoed back.
     * @param[in] side_str The order's Side as its FIX value, echoed back; empty to leave tag 54 out.
     * @param[in] order_qty The order's OrderQty, echoed back; empty to leave tags 38, 151 and 14 out.
     * @param[in] reason Text for tag 58.
     * @param[in] ord_rej_reason Value for tag 103.
     */
    void send_reject_execution_report(FixSession& session, std::string_view cl_ord_id, std::string_view symbol, std::string_view side_str,
                                      std::string_view order_qty, std::string_view reason, pubsub_itc_fw_app::OrdRejReason ord_rej_reason);

    /**
     * @brief Refuses a request to cancel, which has not reached the matching engine, with a session-level Reject (35=3).
     *
     * The reply refuses the request and says nothing of the order it names: no OrderID and no order status, since the
     * component holding the order never saw the request (R-0151, docs/venue/gateway_refusals.md). A request the matching
     * engine refuses is answered by the engine, with an OrderCancelReject that reports the order's status. The session
     * stays open (R-0150).
     *
     * Tag values follow FIX 5.0 SP2:
     *   - 35  = 3          (Reject)
     *   - 45  = the request's MsgSeqNum (RefSeqNum)
     *   - 372 = F          (RefMsgType: an OrderCancelRequest)
     *   - 371 = @p ref_tag (RefTagID), only when one field is at fault
     *   - 373 = @p reason  (SessionRejectReason)
     *   - 58  = @p text
     *
     * @param[in,out] session The member's session. Its outbound seq num is advanced.
     * @param[in] inbound The refused OrderCancelRequest. String_views in it are valid for this call only.
     * @param[in] reason The kind of refusal.
     * @param[in] text The reason in words, for tag 58.
     * @param[in] ref_tag The tag of the field at fault; 0 when no one field is at fault.
     */
    void refuse_cancel(FixSession& session, const ParsedFixMessage& inbound, fix_common::RefusalReason reason, std::string_view text, int ref_tag = 0);

    /**
     * @brief The same Reject, for a request to cancel that was sent to the sequencers and refused by the leading one, so
     *        that the member's FIX message is no longer to hand.
     *
     * The refused message is named by the MsgSeqNum the gateway put on the command's envelope, which the sequencer returns
     * on CommandRefused.
     *
     * @param[in,out] session The member's session. Its outbound seq num is advanced.
     * @param[in] member_msg_seq_num The refused message's MsgSeqNum; 0 if the refusal did not carry it.
     * @param[in] cl_ord_id The request's ClOrdID, for the Quill log.
     * @param[in] reason The kind of refusal.
     * @param[in] text The reason in words, for tag 58.
     */
    void refuse_cancel(FixSession& session, int64_t member_msg_seq_num, std::string_view cl_ord_id, fix_common::RefusalReason reason, std::string_view text);

    /**
     * @brief Asks the session's throttle whether a member command of kind @p command may go on.
     *
     * Logs when the session starts being throttled for that kind and when it stops, not each
     * refusal, and counts each refusal in the gateway's metrics. The caller refuses a command
     * this returns false for, with the session's refusal text for the kind.
     *
     * @param[in,out] session The session the command arrived on.
     * @param[in]     command The kind of command.
     * @return True if the command is within the session's limit, or the kind has no limit.
     */
    [[nodiscard]] bool admit_throttled_command(FixSession& session, fix_common::ThrottledCommand command);

    /**
     * @brief Logs the end of any run of throttle refusals still open when a session ends.
     *
     * A run otherwise ends with the next accepted command, and a session that ends while throttled
     * sends none, so without this the log would say it started being throttled and never stopped.
     *
     * @param[in] session The session that is ending.
     */
    void log_throttling_ended_by_disconnection(const FixSession& session);

    void send_business_reject(FixSession& session, const ParsedFixMessage& inbound, const std::string& reason);

    // Sends a FIX Reject (35=3) for a well-framed inbound message that failed
    // dictionary validation, carrying the session-reject reason and the specific
    // offending tag from the FixReject. Session-level; used only once the FIX
    // session is established (an invalid Logon is disconnected instead).
    void send_fix_reject(FixSession& session, const ParsedFixMessage& inbound, const fix_codec::FixReject& reject);

    // Forward a DSL PDU to the primary sequencer, and to the secondary as well when high
    // availability is on. Only the leading sequencer acts on it; the follower discards its copy.
    // Template so it works with any DSL message type generated by the DSL tool.
    template <typename MsgT> void forward_pdu_to_sequencers(int16_t pdu_id, const MsgT& msg) {
        if (sequencer_primary_conn_id_.get_value() != 0) {
            // A member is waiting for this: it is the order on its way to be matched.
            send_pdu(sequencer_primary_conn_id_, pdu_id, 0, msg, pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "FixOrderGatewayThread: primary sequencer not connected -- PDU not forwarded to primary");
        }
        if (config_.ha_enabled) {
            if (sequencer_secondary_conn_id_.get_value() != 0) {
                send_pdu(sequencer_secondary_conn_id_, pdu_id, 0, msg, pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
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
    //            socket. Zero is stamped as absent, so a command with no such time never
    //            contributes a round trip that no client waited for.
    // @param[in] member_msg_seq_num The MsgSeqNum of the member's FIX message that carried the
    //            command, or 0 if it is not known. The sequencer returns it if it refuses the
    //            command, so that the refusal can name the message (docs/venue/gateway_refusals.md).
    // @return The refusal the caller must send the member, if the command was not sent: the store of unanswered
    //         commands is full, or a command with the same comp id and ClOrdID is already waiting for its answer.
    template <typename MsgT>
    [[nodiscard]] fix_common::GatewayRefusal forward_order_in_envelope(int16_t inner_pdu_id, const MsgT& msg, int32_t gateway_session_conn_id,
                                                                       std::string_view sender_comp_id, int64_t gateway_ingress_ns,
                                                                       int64_t member_msg_seq_num) {
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
            // Logged as the error it is. Not a reason to tell the caller the venue is busy.
            return fix_common::GatewayRefusal::None;
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
        // The command's ClOrdID, so the sequencer can keep its record of the identifiers in its log
        // without decoding the command (docs/availability/commands_during_a_change_of_leader.md, 3.4).
        envelope.has_cl_ord_id = !msg.cl_ord_id.empty();
        envelope.cl_ord_id = msg.cl_ord_id;
        envelope.has_member_msg_seq_num = member_msg_seq_num > 0;
        envelope.member_msg_seq_num = member_msg_seq_num;
        if (!sender_comp_id.empty()) {
            envelope.has_sender_comp_id = true;
            envelope.sender_comp_id = sender_comp_id;
        }

        // The gateway's own cost: decoding the client's bytes, validating the fields and
        // building this envelope. This is the part of the round trip that differs between
        // the two protocols; everything after it is work they share. Recorded for a
        // NewOrderSingle only, which is the population the round-trip histogram measures,
        // so the two can be read against each other without correcting for message mix.
        //
        // Skipped without an ingress stamp, on the same reasoning as the round trip.
        if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) && gateway_ingress_ns != 0) {
            const int64_t ingress_to_forward_ns = config_.wall_clock->now_ns() - gateway_ingress_ns;
            if (ingress_to_forward_ns >= 0) {
                order_ingress_to_forward_histogram_.observe(static_cast<double>(ingress_to_forward_ns));
            }
        }

        // A copy is kept until a report answers the command, so that it can be sent again to a new
        // sequencer leader (docs/availability/commands_during_a_change_of_leader.md, section 3.1).
        const fix_common::GatewayRefusal refusal = keep_until_answered(envelope, sender_comp_id, msg.cl_ord_id);
        if (refusal != fix_common::GatewayRefusal::None) {
            return refusal;
        }
        forward_pdu_to_sequencers(pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope);
        return fix_common::GatewayRefusal::None;
    }

    // Keeps a copy of a command's envelope until it is answered. Returns GatewayRefusal::None when the copy was kept,
    // and when the store is not in use because high availability is off; otherwise why it could not be kept.
    fix_common::GatewayRefusal keep_until_answered(const pubsub_itc_fw_app::WalRecord& envelope, std::string_view comp_id, std::string_view cl_ord_id);
    // A connection to a sequencer has written everything waiting on it: the next batch of commands being sent again
    // goes (pubsub_itc_fw::UnansweredCommands).
    void on_connection_writable(pubsub_itc_fw::ConnectionID id) override;

    // How the library's store of unanswered commands sends a kept command again: decoded, marked as sent again, and
    // sent on the sequencer connection it names.
    class ResendLinks : public pubsub_itc_fw::UnansweredCommandLinksInterface {
      public:
        explicit ResendLinks(FixOrderGatewayThread& owner) : owner_(owner) {}
        void send_again(const pubsub_itc_fw::ConnectionID& connection, const uint8_t* kept, size_t size) override;
        void request_writable_notification(const pubsub_itc_fw::ConnectionID& connection) override;

      private:
        FixOrderGatewayThread& owner_;
    };
    ResendLinks resend_links_{*this};
    // The sequencers, in the order they are named to the store of unanswered commands.
    static constexpr size_t primary_sequencer_destination = 0;
    static constexpr size_t secondary_sequencer_destination = 1;

    // The test events this thread sends, when a test started this gateway (docs/availability/test_events.md).
    class GatewayTestEventLinks : public pubsub_itc_fw::TestEventLinksInterface {
      public:
        explicit GatewayTestEventLinks(FixOrderGatewayThread& owner) : owner_(owner) {}
        void send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override;

      private:
        FixOrderGatewayThread& owner_;
    };
    GatewayTestEventLinks test_event_links_{*this};
    std::optional<pubsub_itc_fw::TestEventChannel> test_events_;

    // Sends one of the gateway's test events, if a test started this gateway.
    template <typename Message> void report(const Message& message) {
        if (test_events_.has_value()) {
            test_events_->send_event(Message::message_pdu_id, message);
        }
    }

    // The commands sent to the sequencers and not yet answered, sent again after a change of sequencer leader or when a
    // sequencer connection is established again. Present only with high availability on, because only then is there
    // more than one sequencer.
    std::optional<pubsub_itc_fw::UnansweredCommands> unanswered_commands_;
    // The envelope of a command being kept is encoded here; grown to the largest seen, then reused.
    std::vector<uint8_t> unanswered_encode_buffer_;
    // A copy being sent again is decoded with this, apart from the arena of the message being handled.
    std::vector<uint8_t> resend_arena_buffer_ = std::vector<uint8_t>(64 * 1024);
    // The highest sequencer leadership epoch an OrderAcceptance has carried; zero before the first.
    int32_t highest_leader_epoch_seen_{0};

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
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
    // Sessions are added and removed by this thread alone (principle F1). Checked where they are.
    pubsub_itc_fw::ThreadConfinementCheck sessions_owner_{"FixOrderGatewayThread sessions"};
#endif

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

    // Announce, down a sequencer link that has just come up, every session this gateway holds.
    //
    // A session still waiting for its numbering is announced as at its logon. A member that logs
    // on while the gateway is between connect retries has its SessionBound dropped for want of
    // anywhere to send it, and nothing else would ever ask again -- the session then hangs until
    // `sequence_state_timeout` opens it on numbering the venue never confirmed. See
    // docs/bug_list.md, BUG-0019.
    //
    // An established session is announced as still connected (SessionBound.still_connected). The
    // sequencer keeps where each session is connected only in memory, so one that has restarted
    // knows of none, and drops every report for a session it does not know of. See
    // docs/bug_list.md, BUG-0182.
    //
    // Sent down the one link named, NOT to both sequencers: a SessionBound that arrives at a
    // leader which already holds this binding is read as the previous session having died,
    // which deliberately raises the resume figure. That is the right answer for a real
    // failover and the wrong one for a retry, so a link that already took the announcement
    // must not be sent it twice.
    void announce_sessions_on_new_link(pubsub_itc_fw::ConnectionID sequencer_conn_id);
    // Announces the sessions down a sequencer link that has just come up: now, or, when a test asks for a delay
    // ([test_events] delay_session_announcements_ms), when @p delay_timer_id fires.
    void announce_sessions_when_due(pubsub_itc_fw::ConnectionID sequencer_conn_id, pubsub_itc_fw::TimerID& delay_timer_id);
    // The timers for announcements a test asked to be delayed, one for each sequencer link.
    pubsub_itc_fw::TimerID announce_to_primary_timer_id_{};
    pubsub_itc_fw::TimerID announce_to_secondary_timer_id_{};

    // The venue's reply to a binding: what it remembers of this session's sequence numbers,
    // so the member's numbering continues across a reconnect instead of restarting at 1.
    void handle_session_bound_ack(const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief The sequencer saying whether the venue can process the orders it is being given.
     *
     * The sequencer knows there is no matching engine and this gateway holds the member
     * relationship; before this PDU there was no path between the two, so the venue went on
     * acknowledging orders it could not act upon. See BUG-0009 and
     * docs/availability/order_acceptance.md.
     *
     * Repeated while the venue is degraded, so it arrives far more often than it changes.
     */
    void handle_order_acceptance(const pubsub_itc_fw::EventMessage& message);
    // The leading sequencer refused a member's command without writing it into the WAL (CommandRefused): the member
    // is answered with a rejection, and the gateway's copy of the command is marked answered.
    void handle_command_refused(const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief Whether the venue last said it could process orders.
     *
     * Recorded and reported; nothing yet refuses on it. Refusing is step 4 of BUG-0009, and
     * landing the transport on its own means a fault in either half shows up by itself rather
     * than tangled with the other.
     *
     * Starts true, which is the assumption that caused the bug -- it is safe here only because
     * the sequencer sends the state on connection rather than waiting for a transition.
     */
    bool venue_accepting_orders_{true};
    // Why the venue is not accepting, as the leading sequencer last said, which decides what a member is told.
    pubsub_itc_fw_app::OrderAcceptanceReason venue_refusal_reason_{pubsub_itc_fw_app::OrderAcceptanceReason::Accepting};
    // The text a member is given when a new order is refused because the venue is not accepting, saying why.
    [[nodiscard]] const char* venue_refusal_text() const;
    // The OrdRejReason of that refusal: ExchangeClosed while trading is halted, as the matching engine says when it refuses
    // an order because trading is halted, and Other otherwise.
    [[nodiscard]] pubsub_itc_fw_app::OrdRejReason venue_refusal_ord_rej_reason() const;

    /// Whether the sequencer reported a session's numbering before the gateway opened the session.
    enum class SessionNumbering {
        ConfirmedBySequencer, ///< the sequencer reported where the session's numbering stands
        NotReportedInTime     ///< the sequencer did not report it in time, and the session was opened without it
    };

    /// Sends the FIX Logon reply and opens the session, once its numbering is settled or the wait for it is over.
    void complete_session_establishment(FixSession& session, SessionNumbering numbering);

    /// Reports to a test that the gateway refused a member's logon (docs/availability/test_events.md).
    void report_logon_refused(const FixSession& session, pubsub_itc_fw_app::LogonRefusal reason, int16_t primary_instance, int16_t backup_instance);

    // A session's missed execution reports, replayed from the sequencer's WAL in answer to a
    // ResendRequest, and the completion that ends the replay. See handle_resend_request.
    void handle_session_replay_record(const pubsub_itc_fw::EventMessage& message);
    void handle_session_replay_complete(const pubsub_itc_fw::EventMessage& message);

    // Asks the sequencers for the reports on the numbers from where the session's resend has reached
    // to the end of its range, under a new request identifier, so that anything still arriving for an
    // earlier request is ignored. Used to start a resend, and to ask again when a new leader
    // announces itself while a resend is still waiting.
    void send_replay_request(FixSession& session);

    // A new sequencer leads: every resend still waiting asks it for the reports it has not yet placed.
    // Only the leader answers a request, so one sent while no instance led, or to a leader that has
    // since stopped, would otherwise never be answered (BUG-0148).
    void send_waiting_replay_requests_again(int32_t new_epoch);

    // Whether a record or completion that arrived on this sequencer connection belongs to the answer the
    // session's resend is taking. The first one for the current request chooses the connection.
    [[nodiscard]] bool takes_replay_answer_from(FixSession& session, const pubsub_itc_fw::ConnectionID& from);

    // Ends a resend: gap-fills what is left of the range, then delivers the live reports held while it ran.
    void finish_replay(FixSession& session, int32_t record_count, bool truncated, int64_t last_seq_no);

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
    bool send_execution_report_to_session(FixSession& session, const pubsub_itc_fw_app::ExecutionReportView& view, bool poss_dup, int64_t orig_sending_time_ns,
                                          bool poss_resend = false);

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

    // A session whose connection has gone, whose member asked for its orders to be cancelled, while its grace period runs.
    struct DeadSession {
        std::string client_comp_id;
        // When the grace period runs out.
        std::chrono::steady_clock::time_point cancel_due{};
    };

    // Sessions whose connection dropped and whose grace period has not yet expired, ordered by cancel_due, earliest
    // first. Each member may have its own grace period, so an entry is inserted in its place rather than appended.
    //
    // A session leaves here one of two ways: the grace timer expires, and its unanswered commands are no longer kept, or
    // the same comp id logs on again in time.
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

    /// Guarantees a health line even when nothing is progressing -- which is exactly when one is
    /// wanted, and exactly when the count-driven line falls silent. See BUG-0009.
    pubsub_itc_fw::TimerID order_progress_timer_id_{};
    std::chrono::steady_clock::time_point last_order_progress_report_{};

    /** @brief Tells the sequencer where each established session's numbering has reached. */
    void report_session_sequence_numbers();
    bool grace_timer_active_{false};

    // Ends every grace_sessions_ entry whose deadline has passed, and rearms for the next, if any.
    void expire_grace_sessions();

    // Arms (or rearms) the grace timer for the front entry's deadline.
    void arm_grace_timer();

    // Discards any grace-period entry belonging to this comp id. Called when a comp id logs on: if it got back inside its
    // window, the venue leaves its orders resting.
    //
    // Returns whether there was such an entry, for logging.
    bool reclaim_grace_session(std::string_view comp_id);

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
    pubsub_itc_fw::HistogramHandle order_ingress_to_forward_histogram_;

    // How much of the round trip had already gone by the time the report reached this
    // thread. Its difference from order_round_trip_nanoseconds is what encoding and sending
    // the report costs -- the outbound half, which no metric could express before, because
    // the report path carries no time origin of its own. See OrderPathMetrics.hpp.
    pubsub_itc_fw::HistogramHandle er_in_elapsed_histogram_;

    /// How many member commands of each kind the sessions' throttles have refused.
    fix_common::ThrottleRefusalMetrics throttle_refusal_metrics_;

    // The running totals below, and the sessions and gap fills, published as counters for programs to read.
    fix_common::GatewayProgressMetrics progress_metrics_;

    // Running totals behind the GW-PROGRESS line. Per-order logging was moved to Debug
    // because it cost roughly a third of this gateway's CPU under load; these keep the
    // counts available at a thousandth of that.
    int64_t orders_received_{0};
    int64_t execution_reports_sent_{0};
    int64_t execution_reports_dropped_{0};
    /// New orders that the matching engine has accepted or rejected. Reports of cancels are not counted here.
    int64_t orders_answered_{0};

    /**
     * @brief Orders refused because the venue said it could not process them, and cancels likewise.
     *
     * Two counters rather than one, because only the first belongs in the accounted total.
     * `orders_received_` counts NewOrderSingle and nothing else, so a refused order has to leave
     * the awaiting pool it entered -- while a refused cancel never entered it, and adding one to
     * the same total would drive `awaiting` negative.
     *
     * A refused order IS accounted for: the member has a definitive answer, which is the whole
     * point of refusing rather than deferring. See BUG-0009 and
     * docs/availability/order_acceptance.md.
     */
    int64_t orders_refused_{0};
    int64_t cancels_refused_{0};
};

} // namespaces
