#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

#include "FixOrderLimits.hpp"
#include "FixParser.hpp"
#include "OpenOrderEntry.hpp"
#include "SeqNumRanges.hpp"

namespace fix_order_gateway {

// Open-order tracking lives in fix_common: cancelling a dead session's resting orders is
// a property of being a gateway, not of speaking FIX, so the binary gateway shares these.
// Re-exported here under the names the FIX gateway has always used.
using open_orders::max_supported_order_qty_length;
using open_orders::max_supported_symbol_length;
using open_orders::OpenOrderEntry;
using open_orders::OpenOrderMap;

/**
 * @brief Holds the state for a single active FIX 5.0SP2 / FIXT 1.1 session.
 *
 * One FixSession is created per accepted inbound connection and destroyed
 * when the connection is lost. It owns the FixParser for that connection's
 * TCP stream -- parsers are stateful (they accumulate bytes between recv()
 * calls) so each connection must have its own independent instance.
 *
 * The FixSerialiser is stateless and is shared across all sessions by
 * FixOrderGatewayThread.
 *
 * Sequence numbers no longer reset to 1 on each new connection. The venue remembers where
 * a session's numbering had reached and hands it back when a gateway binds the session, so
 * a reconnect continues the member's numbering rather than restarting it -- including a
 * reconnect to a different gateway instance, which is the case it exists for. A member that
 * wants to start again says so with ResetSeqNumFlag=Y on its Logon, and that is honoured.
 *
 * What is deliberately NOT held here is a store of the messages sent. Recovering them is the
 * sequencer's job, from its WAL, because the reports may have been sent by a different
 * instance of this gateway entirely. See docs/availability/gateway_ha.md.
 */
struct FixSession {
    /**
     * @brief Constructs a FixSession for the given connection.
     *
     * @param[in] id         The ConnectionID assigned by the reactor.
     * @param[in] logger     Logger instance. Must outlive this object.
     * @param[in] on_message Called by the parser for each valid FIX message.
     * @param[in] on_reject  Called for a well-framed message that fails validation.
     */
    FixSession(pubsub_itc_fw::ConnectionID id, pubsub_itc_fw::QuillLogger& logger, FixParser::MessageCallback on_message, FixParser::RejectCallback on_reject)
        : conn_id(id), parser(logger, std::move(on_message), std::move(on_reject)) {}

    // Not copyable -- FixParser holds a std::function (non-copyable).
    FixSession(const FixSession&) = delete;
    FixSession& operator=(const FixSession&) = delete;

    // Moveable for unordered_map insertion.
    FixSession(FixSession&&) = default;
    FixSession& operator=(FixSession&&) = default;

    // Identity

    pubsub_itc_fw::ConnectionID conn_id;

    // Parser -- one per connection.
    //
    // The parser itself is stateless (no internal accumulation buffer).
    // Partial-message bytes are preserved in the connection's MirroredBuffer by
    // committing only fully consumed bytes after each call to feed(). A separate
    // parser per connection is still required because each connection has its own
    // MirroredBuffer and its own in-flight partial message state.

    FixParser parser;

    // Session state

    /**
     * @brief True once the inbound byte stream has been verified to start
     *        with the expected FIX preamble (8=FIXT.1.1<SOH>).
     *
     * Checked in on_raw_socket_message() before bytes are fed to the parser.
     * A connection whose first bytes do not match the preamble is disconnected
     * immediately without any FIX-level response.
     */
    bool preamble_verified{false};

    /**
     * @brief True once a valid Logon (MsgType=A) has been received and
     *        the gateway has responded with its own Logon.
     */
    bool session_established{false};

    /**
     * @brief True when the client sent a FIX Logout rather than the socket simply dying.
     *
     * Cancel-on-disconnect reads this: a member that logs out has said what it wants, so
     * its resting orders are cancelled at once with no grace period. A socket that
     * vanished has said nothing, and gets the full window to reconnect.
     */
    bool clean_logout{false};

    /**
     * @brief This comp id's cancel-on-disconnect settings, from AuthenticationResult.
     *
     * Empty means the member has no stored preference and the gateway's own
     * [cancel_on_disconnect] configuration applies. That is not the same as false or zero:
     * an operator who raises the venue-wide window must not have to revisit every member,
     * so silence stays distinguishable from a deliberate value the whole way from the
     * database column to this struct.
     */
    std::optional<bool> cancel_on_disconnect_enabled;
    std::optional<int32_t> cancel_on_disconnect_grace_period_seconds;

    /**
     * @brief True while a SCRAM-SHA-256 authentication exchange is in progress
     *        for this session. The FIX session is not established until the
     *        exchange completes with Granted and the ServerSignature is verified.
     */
    bool auth_pending{false};

    /** @brief Client nonce generated by the gateway for this session's SCRAM exchange. */
    std::vector<uint8_t> scram_client_nonce;

    /** @brief Expected ServerSignature computed locally; verified against the AuthenticationResult. */
    std::vector<uint8_t> scram_expected_server_signature;

    /** @brief HeartBtInt value extracted from the Logon message; used when completing the session. */
    int heartbeat_interval{30};

    /**
     * @brief Password extracted from tag 554 of the inbound Logon message.
     *
     * Held only for the duration of the SCRAM exchange: zeroed and cleared as
     * soon as the AuthenticationProof has been sent.  Never logged.
     */
    std::string client_password;

    /**
     * @brief The SenderCompID sent by the client in the Logon message.
     *
     * Used to validate subsequent messages and to set TargetCompID in
     * outbound messages to this client.
     */
    std::string client_comp_id;

    /**
     * @brief Outbound sequence number for messages sent to this client.
     * Incremented by FixGatewayThread::send_fix_to_session().
     */
    int outbound_seq_num{1};

    /**
     * @brief The next sequence number the venue expects to receive FROM this member.
     *
     * The highest `MsgSeqNum` seen on this session plus one. Never lowered: a number below what
     * has already been seen describes a message the venue has processed, and winding the counter
     * back would make it ask for messages again.
     *
     * **Observed but not yet acted on.** Nothing compares an arriving number against this, so a
     * member that skips numbers is still processed as though nothing were missing -- BUG-0038, of
     * which this field is the first step. What it does buy immediately is that the venue now
     * *knows* where a member's numbering stands and remembers it across a gateway change, which
     * is the state the checking will need before it can be written.
     *
     * Seeded from `SessionBoundAck`, reported onward on `SessionSequenceUpdate` and
     * `SessionUnbound`, exactly as `outbound_seq_num` is -- and resumed differently from it. See
     * docs/fix/inbound_sequence_checking.md.
     */
    int expected_inbound_seq_num{1};

    /**
     * @brief True when the member asked, on Logon, for both sides to restart at 1.
     *
     * ResetSeqNumFlag=Y is the standard way a member says "forget where we were". A venue
     * must honour it, and honouring it means the sequence numbers the venue remembered for
     * this session are deliberately discarded rather than restored -- and there is nothing
     * to resend, because by the member's own account nothing is missing.
     *
     * It matters here because the two features pull in opposite directions: continuing a
     * member's numbering across a reconnect is what step 6 is for, and this is the member
     * explicitly declining it. A client that resets and a venue that insists on continuing
     * would deadlock into a resend loop neither side can end.
     */
    bool reset_seq_num_requested{false};

    /**
     * @brief True between binding the session and hearing where its numbering stands.
     *
     * The Logon reply is a numbered message, so it cannot be sent until the venue has said
     * what number it should carry. Between those two points the session is authenticated but
     * not yet established, and nothing may be sent to the member.
     */
    bool awaiting_sequence_state{false};
    pubsub_itc_fw::TimerID sequence_state_timeout_timer_id{};

    /**
     * @brief State of a resend in progress for this session, if any.
     *
     * A member that has missed reports asks for them with a ResendRequest, and the reports
     * come back from the sequencer asynchronously -- they are in its WAL, not in this
     * process, because a gateway holds no store of what it has sent and the gateway that
     * sent them may not even be this one.
     *
     * Live reports arriving mid-resend must not be interleaved with the replayed ones: the
     * member is being handed a numbered sequence, and a live report slotted into the middle
     * of it would take a sequence number the resend still needs. So they are held in
     * deferred_execution_reports until the replay ends, then delivered in order behind it.
     * The window is short -- one round trip to the sequencer -- but it is not zero.
     */
    bool replay_in_progress{false};
    int64_t replay_request_id{0};
    /// The outbound number the session had reached before the resend began; where to resume.
    int replay_resume_seq_num{1};
    /// The last number the member asked for: EndSeqNo when it named one, otherwise the last
    /// number the session had reached. Nothing is resent past it.
    int replay_end_seq_num{1};

    /**
     * @brief Which of this session's outbound numbers held an execution report.
     *
     * A resend rewinds the outbound number and refills the range from the sequencer's WAL,
     * which holds reports and nothing else. Every other message the member was sent -- a Logon,
     * a heartbeat, a reject, a report this gateway synthesised for an order the sequencer never
     * saw -- took a number in that range and cannot be produced again, so those numbers are
     * gap-filled instead. This says which they are; a number not covered here is gap-filled,
     * and it makes no difference whether that is because nothing replayable was there or
     * because the venue no longer remembers back that far.
     *
     * Seeded from SessionBoundAck rather than accumulated here from scratch, which is the
     * point: a resend is served by whichever instance holds the session now, and after a
     * failover that is not the instance that sent the messages being asked about. The
     * sequencer holds the record across that change, exactly as it holds outbound_seq_num.
     *
     * Reported onward on SessionSequenceUpdate and SessionUnbound, from
     * report_seq_nums_shipped_to, so an update carries what has happened since the last one
     * rather than the session's whole history.
     *
     * See docs/availability/resend_provenance.md, and BUG-0051 for what refilling every number
     * with a report instead does to a member.
     */
    std::vector<fix_common::SeqNumRange> report_seq_nums;
    /// Highest number already reported to the sequencer; where the next update starts.
    int report_seq_nums_shipped_to{0};
    /// Encoded ExecutionReports that arrived live while the resend was running.
    std::vector<std::vector<uint8_t>> deferred_execution_reports;

    /**
     * @brief Counter for generating unique OrderID values for this session.
     */
    int order_id_counter{1};

    /**
     * @brief Counter for generating unique ExecID values for this session.
     */
    int exec_id_counter{1};

    /**
     * @brief Counter used to generate unique ClOrdID values for gateway-initiated
     *        OrderCancelRequests sent on client disconnect.
     */
    int cancel_id_counter{1};

    /**
     * @brief Orders forwarded to the sequencer that have not yet received a
     *        terminal ExecutionReport (Filled, Canceled, Rejected, Expired,
     *        DoneForDay).  Keyed by ClOrdID.  On client disconnect, one
     *        OrderCancelRequest is sent to the ME for each entry so that orders
     *        do not remain live on the book after the originating session is gone.
     */
    OpenOrderMap open_orders;

    // Raw-bytes commit bookkeeping
    //
    // Each on_raw_socket_message event carries:
    //   - payload() and payload_size() -- the bytes currently visible in
    //     the MirroredBuffer, starting at the tail_position recorded at
    //     enqueue time. payload_size() is CUMULATIVE: if the reactor reads
    //     more data before a previous commit_raw_bytes has been processed,
    //     the next event reports a larger size that INCLUDES the bytes
    //     already handed to the application.
    //   - tail_position() -- the MirroredBuffer tail at enqueue time.
    //
    // The receiver must consume and commit only the bytes that have not yet
    // been seen. A naive "compare tail_position to last_seen_tail" approach
    // is NOT sufficient, because the tail can advance partially (one of our
    // earlier commits has landed but the next one hasn't) while we have
    // already fed bytes past the new tail to the parser.
    //
    // The correct invariant is in absolute byte-stream offsets:
    //
    //   absolute_head_seen_      = max(event_tail_position + event_payload_size)
    //                              across all events seen
    //   absolute_bytes_committed_ = sum of all bytes asked to commit
    //
    // On each event:
    //   1. Update absolute_head_seen_ from the event.
    //   2. The bytes to consume = absolute_head_seen_ - absolute_bytes_committed_.
    //   3. They live at offset (absolute_bytes_committed_ - event_tail_position)
    //      within the visible window starting at message.payload().
    //   4. Feed those bytes to the parser and commit them. Update
    //      absolute_bytes_committed_.
    //
    // For the bursts this gateway sees (single fix8 client, max ~64 KB of
    // unacknowledged FIX text), these counters do not overflow and the
    // MirroredBuffer never wraps in the absolute byte-offset sense, so this
    // simple linear math works. If wrap-around becomes possible in future,
    // the trackers need a modulo-aware comparison.
    int64_t absolute_head_seen_{0};
    int64_t absolute_bytes_committed_{0};

    // Per-session timer ids (default-constructed = not scheduled). The gateway
    // starts, cancels, and recognises each session's timers by these ids.
    pubsub_itc_fw::TimerID logon_timeout_timer_id{};
    pubsub_itc_fw::TimerID scram_auth_timeout_timer_id{};
};

} // namespaces
