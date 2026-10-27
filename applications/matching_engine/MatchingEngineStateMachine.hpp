#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <functional>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <matching_engine_test_events.hpp>

#include "OrderBook.hpp"
#include "OrderEntry.hpp"
#include "ReportIdentifiers.hpp"
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
 * A command is applied by the same code whichever it is, so that the book and the reports are the same.
 * These are the only differences. A command applied live is checked against the order of sequence
 * numbers; a new order it puts on the book is replicated to the follower engine and counted as
 * processed. A command applied during a catch-up is not checked against the order of sequence numbers,
 * whose records are below the position by construction, and its reports are marked as possible repeats.
 * A command a catch-up delivers that this engine has already acted on, by its sequence number, is not
 * applied again. A new order is refused while trading is halted whichever way it arrives, because the
 * halt is set by a command at a position in the WAL, and an engine catching up meets it at the same
 * position as the engine that applied it live.
 */
enum class CommandSource { live, catch_up };

/// Whether a CancelBook command also halts trading if it cancels any order. See MatchingEngineStateMachine::apply_cancel_book.
enum class HaltIfOrdersCancelled { no, yes };

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
     *                            command produced, such as a cancel of every order on promotion.
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
     * @brief A live command halted trading or lifted a halt. Not called for a command applied during a catch-up.
     *
     * The follower engine does not apply commands, so it is told of a halt as it is told of a change to the book, and a
     * follower promoted later refuses orders as the engine it follows does.
     * @param[in] sequence_number The command's sequence number.
     * @param[in] halted          Whether trading is now halted.
     */
    virtual void trading_halt_changed(int64_t sequence_number, bool halted) = 0;

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
 * state machine is given a reference. It is given the maker of identifiers too, which holds the
 * secret the engine read at start-up. Every OrderID and ExecID it produces is made from the
 * position of a command in the WAL, with keys derived from the trading date the command carries
 * (docs/availability/deterministic_matching_engine.md, section 3). What the state machine owns is
 * what only the handling of commands uses: the highest sequence number acted on, and whether
 * trading is halted.
 *
 * Not thread-safe. It belongs to the matching engine's thread, as the book does.
 */
class MatchingEngineStateMachine {
  public:
    /**
     * @param[in,out] book        The book the commands are applied to. Must outlive this object.
     * @param[in,out] identifiers What makes the OrderID and ExecID values. Must outlive this object.
     * @param[in,out] outputs     Where the reports and the changes to the book go. Must outlive this object.
     * @param[in]     logger      Where to log. Must outlive this object.
     */
    MatchingEngineStateMachine(OrderBook& book, ReportIdentifiers& identifiers, MatchingEngineOutputsInterface& outputs, pubsub_itc_fw::QuillLogger& logger);

    MatchingEngineStateMachine(const MatchingEngineStateMachine&) = delete;
    MatchingEngineStateMachine& operator=(const MatchingEngineStateMachine&) = delete;

    /**
     * @brief Applies a new order.
     *
     * The order is refused if it carries no time or no trading date, if its ClOrdID is longer than the book's key holds,
     * if the session already has an open order with that ClOrdID, or if the book's region is full, and
     * applied live it is refused too if its sequence number is not above the highest acted on or while
     * trading is halted; otherwise it is put on the book and acknowledged. The differences between live
     * and a catch-up are those of CommandSource. The book is published to the order's sequence number
     * once its report has been sent: live, only when the order was put on the book; during a catch-up,
     * after every command.
     *
     * A failure while the order is applied, a C++ exception thrown by anything it reaches, costs this
     * order and not the engine. Where the order had not yet changed the book or been answered, it is
     * rejected to its member as one the venue could not process; where it had, it stands as far as it
     * went and nothing more is sent. Either way the failure is reported at Error with the sequence
     * number, and the next command is applied as usual (docs/availability/deterministic_matching_engine.md,
     * section 6).
     *
     * @param[in] view            The order.
     * @param[in] sequence_number Its sequence number in the sequencer's WAL.
     * @param[in] transact_time   The time the sequencer took it, in nanoseconds since the epoch, from its WAL record,
     *                            written on every report. Zero means the record carries no time, which is a defect in
     *                            the sequencer, and the command is refused.
     * @param[in] trading_date    The trading date its WAL record carries, as the number yyyymmdd, which the keys of the
     *                            identifiers are derived from. Zero means the record carries none, which is a defect in
     *                            the sequencer, and the command is refused.
     * @param[in] session         The session that placed it, which with the ClOrdID identifies it on the book.
     * @param[in] source          Whether it is applied live or during a catch-up.
     */
    void apply_new_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                         const fix_common::SessionIdentity& session, CommandSource source);

    /**
     * @brief Applies a request to cancel an order.
     *
     * The request is refused if it carries no time or no trading date, if its ClOrdID or OrigClOrdID is longer than the
     * book's key holds, or if the book holds no such order, and applied live it is refused too if its
     * sequence number is not above the highest acted on; otherwise the order is taken off the book and
     * the cancel reported, describing the order from what the book held. The differences between live
     * and a catch-up are those of CommandSource.
     *
     * A failure while it is applied is dealt with as for apply_new_order.
     *
     * Parameters as for apply_new_order.
     */
    void apply_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                      const fix_common::SessionIdentity& session, CommandSource source);

    /// What a CancelBook command did.
    struct BookCancelledByCommand {
        bool applied{false};     ///< false when the command was not applied: one a catch-up delivers again, or out of sequence
        int64_t open_orders{0};  ///< how many orders the book held when the command was applied
        int64_t reports_sent{0}; ///< how many orders were cancelled, each with a report
        int64_t orders_kept{0};  ///< how many orders, placed after the command's position, stay on the book
        bool halted{false};      ///< true when the command halted trading, because it asked to if it cancelled any order and it did
    };

    /**
     * @brief Applies a CancelBook command from the WAL: cancels every order placed at or before its position, in the
     * order they were placed, reporting each to the member that placed it. Orders placed after the position stay.
     *
     * A command, not a decision of this engine's, so every engine that applies the WAL cancels the same orders with the
     * same reports, including one that catches up later (docs/availability/deterministic_matching_engine.md, section 4).
     * Each report's ExecID is made from the command's sequence number and the report's place, from 0; the reports are
     * handed to the outputs with sequence number 0, so that the sequencer routes each by the session on its envelope.
     * Live, each order removed is passed to order_removed_from_book, so that the follower engine's copy of the book
     * loses it too. The book is published to the command's sequence number afterwards.
     *
     * A command at or below the highest acted on is not applied: during a catch-up because it was applied already, and
     * live because the WAL the engine acted on and the one now sending disagree, as for an order. A command without a
     * time or a trading date is a defect in the sequencer; it is applied all the same, with what it carries, because it
     * has no member to refuse it to, and reported at Error.
     *
     * With halt_if_orders_cancelled, a command that cancels any order halts trading as well, which is what R-0117 asks
     * of the venue after no engine could trade for too long: the orders were cancelled because their members were locked
     * out of them, and trading stays halted until a person lifts it. One that cancels nothing leaves trading as it was,
     * because no member was left holding an order it could not reach. The halt is recorded in the region before the
     * position is published, so an engine that restarts later finds it there.
     *
     * @param[in] orders_placed_through   The command's position: orders placed at or before it are cancelled.
     * @param[in] halt_if_orders_cancelled Whether to halt trading if the command cancels any order.
     * @param[in] sequence_number         The command's sequence number.
     * @param[in] transact_time           The time the sequencer recorded for it, written on every report.
     * @param[in] trading_date            The trading date it carries.
     * @param[in] source                  Whether it is applied live or during a catch-up.
     * @return What it did.
     */
    BookCancelledByCommand apply_cancel_book(int64_t orders_placed_through, HaltIfOrdersCancelled halt_if_orders_cancelled, int64_t sequence_number,
                                             int64_t transact_time, int32_t trading_date, CommandSource source);

    /// What a TechnicalEvent command did.
    struct TechnicalEventApplied {
        bool applied{false};      ///< false when the command was not applied: one a catch-up delivers again, out of sequence, or for an older day
        bool halt_changed{false}; ///< true when it halted trading that was not halted, or lifted a halt
    };

    /**
     * @brief Applies a TechnicalEvent command from the WAL: the venue's phase, whether trading is halted, and the trading
     * day (docs/venue/trading_phases.md).
     *
     * The event says the state the venue is now in, so the engine takes whether it is halted from it, whatever it was
     * before. While halted, every live new order is refused and every cancel is still carried out. The phase is not
     * acted on, because the venue has only the open phase until the start and the end of the trading day are built.
     *
     * An event whose trading day is older than the trading date of the record that carries it is refused and reported
     * at Error, so that a halt declared for one day cannot stop trading on a later one. A command at or below the
     * highest acted on is not applied, as for CancelBook. Nothing is sent to any member.
     *
     * @param[in] view            The event.
     * @param[in] sequence_number The command's sequence number.
     * @param[in] trading_date    The trading date of the record that carries it.
     * @param[in] source          Whether it is applied live or during a catch-up.
     * @return What it did.
     */
    TechnicalEventApplied apply_technical_event(const pubsub_itc_fw_app::TechnicalEventView& view, int64_t sequence_number, int32_t trading_date,
                                                CommandSource source);

    /// Whether trading is halted. Kept in the region with the orders, so it is as the commands left it.
    [[nodiscard]] bool halted() const {
        return book_.halted();
    }

    /**
     * @brief Tells the state machine that a catch-up has brought the book to a sequence number, so that a live
     * command at or below it is refused as out of sequence.
     * @param[in] sequence_number The last sequence number the catch-up applied.
     */
    void note_caught_up_to(int64_t sequence_number);

    /// The highest sequence number of any command applied, live or by a catch-up, or the position a completed catch-up
    /// brought the book to, whichever is higher.
    [[nodiscard]] int64_t highest_applied() const {
        return highest_applied_seq_no_;
    }

  private:
    // Halts trading, without answering, for a command whose sequence number an identifier cannot hold.
    void refuse_to_name(int64_t sequence_number, CommandSource source);

    // Records whether trading is halted, before the command's position is published, and tells the follower engine
    // when a live command changed it.
    void set_halted(TradingHalted halted, int64_t sequence_number, CommandSource source);

    // The work of apply_new_order and apply_cancel, which catch what it throws.
    void apply_new_order_unguarded(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                                   const fix_common::SessionIdentity& session, CommandSource source);
    void apply_cancel_unguarded(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                                const fix_common::SessionIdentity& session, CommandSource source);

    // Starts the count of what one command has done, which after_failure reads.
    void begin_command();

    // Sends a report produced by a command, counting it.
    void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& session,
                     ReportIsRepeat repeat);

    /**
     * @brief Deals with a failure while one command was applied: rejects the command to its member if it had changed
     * nothing and answered nothing, and reports the failure at Error either way.
     *
     * @param[in] sequence_number The command's sequence number.
     * @param[in] source          Whether it was applied live or during a catch-up.
     * @param[in] what            What failed.
     * @param[in] send_rejection  Sends the rejection, given a buffer for its ExecID, which outlives the call.
     */
    void after_failure(int64_t sequence_number, CommandSource source, const char* what, const std::function<void(ReportIdentifiers::Buffer&)>& send_rejection);

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
     * Raises highest_applied_seq_no_ to sequence_number when it returns false.
     */
    [[nodiscard]] bool sequence_goes_backwards(int64_t sequence_number);

    /**
     * @brief Whether a command delivered by a catch-up is one this engine has already applied, and so is not applied again.
     *
     * True when its sequence number is at or below the highest applied, live or by a catch-up, complete or not. When it
     * is false, the command is about to be applied, and its sequence number becomes the highest applied. Every command is
     * applied once, in the order of its sequence number, whichever way it arrives (BUG-0212).
     */
    [[nodiscard]] bool applied_before(int64_t sequence_number);

    // Publishes the book to a refused command's sequence number during a catch-up, and not live. See the definition for why.
    void publish_after_refusal(int64_t sequence_number, CommandSource source);

    /**
     * @brief Refuses a new order whose WAL record carries no time or no trading date, which is a defect in the sequencer.
     *
     * The report's TransactTime is what the command carried, zero where it carried no time, so that
     * every engine and every replay produce the same report.
     *
     * @param[in] view            The order.
     * @param[in] sequence_number Its sequence number.
     * @param[in] transact_time   The time it carried, zero if none.
     * @param[in] session         The session that placed it.
     * @param[in] repeat          Whether the report may repeat one an earlier engine sent.
     */
    void refuse_order_without_time(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                   const fix_common::SessionIdentity& session, ReportIsRepeat repeat);

    /**
     * @brief Refuses a request to cancel whose WAL record carries no time or no trading date. The report carries the
     * OrigClOrdID, so each gateway sends it as an OrderCancelReject.
     *
     * Parameters as for refuse_order_without_time.
     */
    void refuse_cancel_without_time(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                    const fix_common::SessionIdentity& session, ReportIsRepeat repeat);

    /**
     * @brief Refuses a new order whose sequence number is not above the last the engine acted on.
     *
     * Its ExecID is made from its sequence number, as every report's is, so where the engine has already
     * answered a command with that number the two reports share an ExecID. That follows from the WAL the
     * engine acted on and the WAL of the sequencer now leading disagreeing, which is the defect this
     * refusal reports, and no identifier made from the position could avoid it.
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
    ReportIdentifiers& identifiers_;
    MatchingEngineOutputsInterface& outputs_;
    pubsub_itc_fw::QuillLogger& logger_;

    // The highest sequence number of any command applied, live or by a catch-up, or the position a completed catch-up
    // brought the book to, whichever is higher. See sequence_goes_backwards() and applied_before().
    int64_t highest_applied_seq_no_{0};

    // What the command being applied has done so far: how many reports it has sent, and whether it has changed the book.
    // after_failure reads them to decide whether a failure can still be answered with a rejection.
    int64_t reports_for_command_{0};
    bool book_changed_by_command_{false};

    // Reusable arena backing the echoed group element arrays of an order's acknowledgement (apply_new_order). Sized to
    // need, grown and retried, rather than a fixed size that could silently drop echoed groups.
    static constexpr size_t initial_er_group_arena_size = 4096;
    static constexpr size_t max_er_group_arena_size = 1u << 20; // 1 MiB sanity ceiling
    std::vector<uint8_t> er_group_arena_buffer_ = std::vector<uint8_t>(initial_er_group_arena_size);
};

} // namespaces
