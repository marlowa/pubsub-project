#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

#include "FixOrderLimits.hpp"
#include "FixParser.hpp"

namespace order_gateway {

// Hard structural ceilings for the symbol/qty open-order string fields (compile-time
// char-array sizes in OpenOrderEntry). Runtime-configurable [fix_limits] limits must be
// <= these and are enforced at startup; over-length symbol/qty get a FIX BusinessReject.
// ClOrdID instead uses the single shared fix_order_limits::max_cl_ord_id_length -- validated
// at ingress with an ExecutionReport rejection -- so the gateway pool and the matching-engine
// book key stay on one value. See docs/applications/order_gateway.md.
//
// Why fixed char arrays rather than std::string or a general pool:
//   std::string causes one heap allocation per field per order, measured at
//   1.21% of gateway CPU at 122K orders/second. A general variable-size pool
//   with individual frees requires implementing boundary-tag coalescing --
//   complexity comparable to ExpandablePoolAllocator, which itself was very
//   hard to write correctly. Fixed char arrays with ExpandablePoolAllocator
//   and tsl::robin_map eliminate all per-order heap allocation with a
//   manageable implementation cost.
static constexpr size_t max_supported_symbol_length = 64;
static constexpr size_t max_supported_order_qty_length = 32;

/**
 * @brief Pool-allocated storage for a single open order's string fields.
 *
 * All string data is held inline in fixed char arrays -- no heap allocation
 * per order. An OpenOrderEntry is allocated from OrderGatewayThread's
 * open_order_pool_ when the ME sends a non-terminal ExecutionReport, and
 * returned to the pool when a terminal ER is received or when
 * drain_pending_cancels() sends the OrderCancelRequest on disconnect.
 *
 * The open_orders map in FixSession holds std::string_view keys that point
 * directly into cl_ord_id[] here. The pool entry must therefore remain alive
 * for the lifetime of the map entry.
 */
struct OpenOrderEntry {
    char cl_ord_id[fix_order_limits::max_cl_ord_id_length + 1]{};
    uint8_t cl_ord_id_len{0};
    char symbol[max_supported_symbol_length + 1]{};
    uint8_t symbol_len{0};
    char order_qty[max_supported_order_qty_length + 1]{};
    uint8_t order_qty_len{0};
    char side{0};
};

/**
 * @brief Map type used for open orders within a session.
 *
 * Key: std::string_view pointing into the corresponding OpenOrderEntry::cl_ord_id[].
 *      The view is stable for the lifetime of the pool entry.
 * Value: non-owning pointer to the pool-allocated OpenOrderEntry.
 *        The owning pool lives in OrderGatewayThread.
 */
using OpenOrderMap = tsl::robin_map<std::string_view, OpenOrderEntry*, std::hash<std::string_view>, std::equal_to<std::string_view>>;

/**
 * @brief Holds the state for a single active FIX 5.0SP2 / FIXT 1.1 session.
 *
 * One FixSession is created per accepted inbound connection and destroyed
 * when the connection is lost. It owns the FixParser for that connection's
 * TCP stream -- parsers are stateful (they accumulate bytes between recv()
 * calls) so each connection must have its own independent instance.
 *
 * The FixSerialiser is stateless and is shared across all sessions by
 * OrderGatewayThread.
 *
 * Sequence numbers reset to 1 on each new connection. A production gateway
 * would persist sequence numbers across connections but that is out of scope
 * for this sample.
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
