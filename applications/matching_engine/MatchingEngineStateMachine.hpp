#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include <fix_orders.hpp>
#include <matching_engine_test_events.hpp>

#include "OrderBook.hpp"
#include "OrderEntry.hpp"
#include "SessionIdentity.hpp"

namespace matching_engine {

/**
 * @brief Whether a report repeats one whose subject the member may already have been told about.
 *
 * Carried to the gateway on the report's envelope and written there as PossResend (tag 97). Every
 * report produced by a catch-up is marked as a possible repeat, because nothing in the record says
 * whether an earlier engine reported it already. See R-0122.
 */
enum class ReportIsRepeat { no, yes };

/**
 * @brief Whether a command is being applied as it arrives or as part of catching up from the sequencer's WAL.
 *
 * A command applied live is checked against the order of sequence numbers, refused while trading is
 * halted, replicated to the follower engine and counted as processed. A command applied during a
 * catch-up is none of those: it is applied to the book, reported as a possible repeat, and, if the
 * book already holds the order, ignored.
 */
enum class CommandSource { live, catch_up };

/**
 * @brief Where the state machine's results go: the reports it produces, and the changes to the book that the
 * follower engine must be told of.
 *
 * MatchingEngineThread implements it, sending the reports to the sequencers and the changes to the
 * follower engine. A unit test implements it by recording what it is given. Every call is made
 * synchronously, from within the call to the state machine that caused it, and every string view it
 * is given is valid only for the duration of the call.
 */
class MatchingEngineOutputsInterface {
  public:
    virtual ~MatchingEngineOutputsInterface() = default;

    /**
     * @brief An execution report to send to the member whose order it concerns.
     * @param[in] report          The report. Its string fields point into buffers that end with the call.
     * @param[in] sequence_number The sequence number of the command that produced it, or 0 for a report no
     *                            command produced, such as a cancel of every order.
     * @param[in] session         The session that placed the order the report concerns.
     * @param[in] repeat          Whether it may repeat a report an earlier engine sent.
     */
    virtual void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& session,
                             ReportIsRepeat repeat) = 0;

    /**
     * @brief A live order was put on the book. Not called for an order applied during a catch-up.
     * @param[in] sequence_number The order's sequence number.
     * @param[in] session         The session that placed it.
     * @param[in] cl_ord_id       Its ClOrdID.
     * @param[in] entry           What the book now holds for it.
     */
    virtual void order_added_to_book(int64_t sequence_number, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                                     const OrderEntry& entry) = 0;

    /**
     * @brief A live cancel took an order off the book. Not called for a cancel applied during a catch-up.
     * @param[in] sequence_number The cancel's sequence number.
     * @param[in] session         The session that placed the order.
     * @param[in] cl_ord_id       The ClOrdID of the order removed, which is the cancel's OrigClOrdID.
     */
    virtual void order_removed_from_book(int64_t sequence_number, const fix_common::SessionIdentity& session, std::string_view cl_ord_id) = 0;

    /**
     * @brief What became of an order or a cancel, for a test that asked to be told.
     * @param[in] session         The session that sent it.
     * @param[in] cl_ord_id       Its ClOrdID.
     * @param[in] outcome         What was done with it.
     * @param[in] repeat          Whether it was applied during a catch-up.
     * @param[in] sequence_number Its sequence number.
     * @param[in] orig_cl_ord_id  For a cancel, the ClOrdID of the order it names; empty for a new order.
     */
    virtual void order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                               ReportIsRepeat repeat, int64_t sequence_number, std::string_view orig_cl_ord_id) = 0;

    /// A live order was accepted onto the book. Called once for each, before order_added_to_book.
    virtual void order_accepted() = 0;
};

/**
 * @brief The matching engine's handling of commands, apart from its connections, its lease agent and its timers.
 *
 * Given a command, with its sequence number, the time the venue took it and the session that sent it,
 * the state machine changes the book and produces the execution reports, and it does nothing else. It
 * reads no clock, opens no connection and keeps no timer: the caller supplies the time, and the
 * results go to a MatchingEngineOutputsInterface. That is what lets it be tested on its own, and it is
 * what phase 7 of the recovery plan needs in order to show that the engine is deterministic
 * (docs/availability/deterministic_matching_engine.md, section 7.1).
 *
 * It does not own the book. The book is a region of mapped memory whose opening, recovery and
 * liveness mark belong to the engine's start-up and timers, so MatchingEngineThread owns it and the
 * state machine is given a reference. What the state machine owns is what only the handling of
 * commands uses: the counters that number orders and reports, the highest sequence number acted on,
 * and whether trading is halted.
 *
 * Not thread-safe. It belongs to the matching engine's thread, as the book does.
 */
class MatchingEngineStateMachine {
  public:
    /// What a cancel of every order did.
    struct BookCancelled {
        int64_t open_orders{0};  ///< how many orders the book held when the cancel began
        int64_t reports_sent{0}; ///< how many cancel reports were produced
    };

    /**
     * @param[in,out] book    The book the commands are applied to. Must outlive this object.
     * @param[in,out] outputs Where the reports and the changes to the book go. Must outlive this object.
     * @param[in]     logger  Where to log. Must outlive this object.
     */
    MatchingEngineStateMachine(OrderBook& book, MatchingEngineOutputsInterface& outputs, pubsub_itc_fw::QuillLogger& logger);

    MatchingEngineStateMachine(const MatchingEngineStateMachine&) = delete;
    MatchingEngineStateMachine& operator=(const MatchingEngineStateMachine&) = delete;

    /**
     * @brief Applies a new order.
     *
     * Live, the order is refused if its sequence number is not above the highest acted on, if its
     * ClOrdID is longer than the book's key holds, while trading is halted, if the session already has
     * an open order with that ClOrdID, or if the book's region is full; otherwise it is put on the book
     * and acknowledged. During a catch-up, an over-long ClOrdID is refused, an order the book already
     * holds is ignored, and any other is put on the book and acknowledged, every report marked as a
     * possible repeat.
     *
     * @param[in] view            The order.
     * @param[in] sequence_number Its sequence number in the sequencer's WAL.
     * @param[in] transact_time   The time the venue took it, in nanoseconds since the epoch, written on every report.
     * @param[in] session         The session that placed it, which with the ClOrdID identifies it on the book.
     * @param[in] source          Whether it is applied live or during a catch-up.
     */
    void apply_new_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                         const fix_common::SessionIdentity& session, CommandSource source);

    /**
     * @brief Applies a request to cancel an order.
     *
     * Live, the request is refused if its sequence number is not above the highest acted on, if its
     * ClOrdID or OrigClOrdID is longer than the book's key holds, or if the book holds no such order;
     * otherwise the order is taken off the book and the cancel reported. During a catch-up, an
     * over-long identifier is refused, a cancel of an order the book does not hold produces no report,
     * and any other takes the order off the book and is reported as a possible repeat.
     *
     * Parameters as for apply_new_order.
     */
    void apply_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                      const fix_common::SessionIdentity& session, CommandSource source);

    /**
     * @brief Cancels every order on the book, reporting each to the member that placed it, and empties the book.
     *
     * The reports carry sequence number 0, because no command produced them, and are visited in the
     * order the book's map holds its entries.
     *
     * @param[in] transact_time The time written on every report.
     * @return How many orders there were, and how many reports were produced.
     */
    BookCancelled cancel_every_order(int64_t transact_time);

    /// Halts trading: from now on every live new order is refused. Nothing lifts a halt.
    void halt() {
        halted_ = true;
    }

    /// Whether trading is halted.
    [[nodiscard]] bool halted() const {
        return halted_;
    }

    /**
     * @brief Tells the state machine the highest order number among the orders recovered from the region at start-up,
     * so that no order accepted later is given a number one of them already has.
     * @param[in] highest_order_id_num The highest order number recovered.
     */
    void note_recovered_orders(int64_t highest_order_id_num);

    /**
     * @brief Tells the state machine that a catch-up has brought the book to a sequence number, so that a live
     * command at or below it is refused as out of sequence.
     * @param[in] sequence_number The last sequence number the catch-up applied.
     */
    void note_caught_up_to(int64_t sequence_number);

    /// The highest sequence number of any live command acted on, or the position a completed catch-up brought the
    /// book to, whichever is higher.
    [[nodiscard]] int64_t highest_applied() const {
        return highest_live_seq_no_;
    }

  private:
    // Formats "ME-ORD-N" or "ME-EXEC-N" into a buffer the caller provides, and returns a view of it. The buffer must
    // outlive the view.
    template <size_t N> static std::string_view format_id(std::array<char, N>& buf, const char* prefix, size_t prefix_len, int64_t counter) {
        std::memcpy(buf.data(), prefix, prefix_len);
        auto [end, ec] = std::to_chars(buf.data() + prefix_len, buf.data() + N, counter);
        return {buf.data(), static_cast<size_t>(end - buf.data())};
    }

    void apply_new_order_from_catch_up(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                       const fix_common::SessionIdentity& session);
    void apply_cancel_from_catch_up(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                    const fix_common::SessionIdentity& session);

    /**
     * @brief Refuses a new order whose ClOrdID is longer than the book's key holds.
     *
     * Sends a rejected report naming the limit. During a catch-up the report is marked as a
     * possible repeat, as every report a catch-up sends is (R-0122).
     *
     * @param[in] view            The order.
     * @param[in] sequence_number Its sequence number.
     * @param[in] transact_time   The time the venue took it.
     * @param[in] session         The session that placed it.
     * @param[in] repeat          Whether the report may repeat one an earlier engine sent.
     */
    void refuse_over_long_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                const fix_common::SessionIdentity& session, ReportIsRepeat repeat);

    /**
     * @brief Refuses a request to cancel whose ClOrdID or OrigClOrdID is longer than the book's key holds.
     *
     * The rejected report carries the OrigClOrdID, so each gateway sends it to the member as an
     * OrderCancelReject (R-0151). The book never holds an order with an identifier that long,
     * so there is no order to report the status of.
     *
     * Parameters as for refuse_over_long_order.
     */
    void refuse_over_long_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                 const fix_common::SessionIdentity& session, ReportIsRepeat repeat);

    /**
     * @brief Whether a live order or cancel carries a sequence number at or below one already acted on.
     *
     * The sequence numbers the leading sequencer sends only go forward, so one that does not means
     * the record the engine has acted on and the WAL of the sequencer now leading disagree. The engine
     * refuses such a command, answering the member, rather than acting on it and holding a book that
     * silently disagrees with the record. Never asked during a catch-up, whose records are below the
     * position by construction. See docs/availability/change_of_sequencer_leader.md, part 4.2.
     *
     * Raises highest_live_seq_no_ to sequence_number when it returns false.
     */
    [[nodiscard]] bool sequence_goes_backwards(int64_t sequence_number);

    /**
     * @brief Refuses a new order whose sequence number is not above the last the engine acted on.
     *
     * Parameters as for refuse_over_long_order, without the repeat marking: this is only ever a live refusal.
     */
    void refuse_out_of_sequence_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                      const fix_common::SessionIdentity& session);

    /**
     * @brief Refuses a request to cancel whose sequence number is not above the last the engine acted on.
     *
     * The report carries the OrigClOrdID, so each gateway sends it as an OrderCancelReject.
     */
    void refuse_out_of_sequence_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                       const fix_common::SessionIdentity& session);

    OrderBook& book_;
    MatchingEngineOutputsInterface& outputs_;
    pubsub_itc_fw::QuillLogger& logger_;

    // Counters for the OrderID and ExecID values the engine generates.
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};

    // The highest sequence number of any live order or cancel acted on, or the position a completed catch-up brought
    // the book to, whichever is higher. See sequence_goes_backwards().
    int64_t highest_live_seq_no_{0};

    // Trading is halted and the venue is not taking orders. R-0117 halts after a long absence; R-0023 requires a
    // person to lift it.
    bool halted_{false};

    // Reusable arena backing the echoed group element arrays of an order's acknowledgement (apply_new_order). Sized to
    // need, grown and retried, rather than a fixed size that could silently drop echoed groups.
    static constexpr size_t initial_er_group_arena_size = 4096;
    static constexpr size_t max_er_group_arena_size = 1u << 20; // 1 MiB sanity ceiling
    std::vector<uint8_t> er_group_arena_buffer_ = std::vector<uint8_t>(initial_er_group_arena_size);
};

} // namespaces
