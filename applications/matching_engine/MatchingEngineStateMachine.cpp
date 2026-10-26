// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineStateMachine.hpp"

#include <algorithm>
#include <exception>
#include <functional>

#include <fmt/format.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include "FixOrderLimits.hpp"
#include "OrderKey.hpp"

namespace matching_engine {

namespace {

// Echo the NoUnderlyings repeating group from a decoded NewOrderSingle onto its
// ExecutionReport. The decoded view carries UnderlyingsView elements; the ER expects
// the (field-identical) Underlyings type, so each element is copied into an array
// allocated from @p arena. The copied string_views still point into the NOS payload
// buffer, which outlives the synchronous ER encode. @p arena and the NOS payload must
// both stay alive until the ER has been encoded.
void echo_underlyings(const pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::UnderlyingsView>& source, pubsub_itc_fw::BumpAllocator& arena,
                      pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::Underlyings>& destination) {
    if (source.size == 0) {
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::Underlyings>(source.size);
    if (elements == nullptr) {
        return; // arena exhausted: leave the ER group empty rather than emit a partial one
    }
    for (size_t index = 0; index < source.size; ++index) {
        const pubsub_itc_fw_app::UnderlyingsView& in = source.data[index];
        pubsub_itc_fw_app::Underlyings& out = elements[index];
        out = pubsub_itc_fw_app::Underlyings{};
        out.has_underlying_symbol = in.has_underlying_symbol;
        out.underlying_symbol = in.underlying_symbol;
        out.has_underlying_security_id = in.has_underlying_security_id;
        out.underlying_security_id = in.underlying_security_id;
        out.has_underlying_qty = in.has_underlying_qty;
        out.underlying_qty = in.underlying_qty;
    }
    destination.data = elements;
    destination.size = source.size;
}

// Echo the NoPartyIDs group (with its nested NoPartySubIDs) from the decoded
// NewOrderSingle onto its ExecutionReport, mirroring echo_underlyings. PartyIDs carries
// a nested list, so the copy descends one level; all element arrays come from @p arena
// and the string_views point into the NOS payload -- both must outlive the ER encode.
void echo_parties(const pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDsView>& source, pubsub_itc_fw::BumpAllocator& arena,
                  pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDs>& destination) {
    if (source.size == 0) {
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::PartyIDs>(source.size);
    if (elements == nullptr) {
        return; // arena exhausted: leave the ER group empty rather than emit a partial one
    }
    for (size_t index = 0; index < source.size; ++index) {
        const pubsub_itc_fw_app::PartyIDsView& in = source.data[index];
        pubsub_itc_fw_app::PartyIDs& out = elements[index];
        out = pubsub_itc_fw_app::PartyIDs{};
        out.has_party_id = in.has_party_id;
        out.party_id = in.party_id;
        out.has_party_id_source = in.has_party_id_source;
        out.party_id_source = in.party_id_source;
        out.has_party_role = in.has_party_role;
        out.party_role = in.party_role;
        if (in.no_party_sub_i_ds.size == 0) {
            continue;
        }
        auto* sub_elements = arena.allocate<pubsub_itc_fw_app::PartySubIDs>(in.no_party_sub_i_ds.size);
        if (sub_elements == nullptr) {
            continue; // arena exhausted: emit the party without its sub-ids
        }
        for (size_t sub = 0; sub < in.no_party_sub_i_ds.size; ++sub) {
            const pubsub_itc_fw_app::PartySubIDsView& sub_in = in.no_party_sub_i_ds.data[sub];
            pubsub_itc_fw_app::PartySubIDs& sub_out = sub_elements[sub];
            sub_out = pubsub_itc_fw_app::PartySubIDs{};
            sub_out.has_party_sub_id = sub_in.has_party_sub_id;
            sub_out.party_sub_id = sub_in.party_sub_id;
            sub_out.has_party_sub_id_type = sub_in.has_party_sub_id_type;
            sub_out.party_sub_id_type = sub_in.party_sub_id_type;
        }
        out.no_party_sub_i_ds.data = sub_elements;
        out.no_party_sub_i_ds.size = in.no_party_sub_i_ds.size;
    }
    destination.data = elements;
    destination.size = source.size;
}

} // un-named namespace

MatchingEngineStateMachine::MatchingEngineStateMachine(OrderBook& book, ReportIdentifiers& identifiers, MatchingEngineOutputsInterface& outputs,
                                                       pubsub_itc_fw::QuillLogger& logger)
    : book_(book), identifiers_(identifiers), outputs_(outputs), logger_(logger) {}

void MatchingEngineStateMachine::note_caught_up_to(int64_t sequence_number) {
    highest_live_seq_no_ = std::max(highest_live_seq_no_, sequence_number);
}

void MatchingEngineStateMachine::apply_new_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                 int32_t trading_date, const fix_common::SessionIdentity& session, CommandSource source) {
    begin_command();
    try {
        apply_new_order_unguarded(view, sequence_number, transact_time, trading_date, session, source);
    } catch (const std::exception& error) {
        after_failure(sequence_number, source, error.what(), [&](ReportIdentifiers::Buffer& exec_id_buf) {
            pubsub_itc_fw_app::ExecutionReport er{};
            er.order_id = "NONE";
            er.exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);
            er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
            er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
            er.symbol = view.symbol;
            er.side = view.side;
            er.leaves_qty = "0";
            er.cum_qty = "0";
            er.avg_px = "0.00";
            er.transact_time = transact_time;
            er.has_cl_ord_id = true;
            er.cl_ord_id = view.cl_ord_id;
            er.has_order_qty = true;
            er.order_qty = view.order_qty;
            er.has_ord_rej_reason = true;
            er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
            er.has_text = true;
            er.text = "Venue could not process the order";
            outputs_.send_report(er, sequence_number, session, source == CommandSource::live ? ReportIsRepeat::no : ReportIsRepeat::yes);
        });
    }
}

void MatchingEngineStateMachine::apply_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                              int32_t trading_date, const fix_common::SessionIdentity& session, CommandSource source) {
    begin_command();
    try {
        apply_cancel_unguarded(view, sequence_number, transact_time, trading_date, session, source);
    } catch (const std::exception& error) {
        after_failure(sequence_number, source, error.what(), [&](ReportIdentifiers::Buffer& exec_id_buf) {
            pubsub_itc_fw_app::ExecutionReport er{};
            er.order_id = "NONE";
            er.exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);
            er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
            er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
            er.symbol = view.symbol;
            er.side = view.side;
            er.leaves_qty = "0";
            er.cum_qty = "0";
            er.avg_px = "0.00";
            er.transact_time = transact_time;
            er.has_cl_ord_id = true;
            er.cl_ord_id = view.cl_ord_id;
            // Carried so the gateways send the report to the member as an OrderCancelReject.
            er.has_orig_cl_ord_id = true;
            er.orig_cl_ord_id = view.orig_cl_ord_id;
            er.has_order_qty = view.has_order_qty;
            er.order_qty = view.order_qty;
            er.has_ord_rej_reason = true;
            er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
            er.has_text = true;
            er.text = "Venue could not process the cancel. The order is unchanged";
            outputs_.send_report(er, sequence_number, session, source == CommandSource::live ? ReportIsRepeat::no : ReportIsRepeat::yes);
        });
    }
}

void MatchingEngineStateMachine::begin_command() {
    reports_for_command_ = 0;
    book_changed_by_command_ = false;
}

void MatchingEngineStateMachine::send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number,
                                             const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    outputs_.send_report(report, sequence_number, session, repeat);
    ++reports_for_command_;
}

void MatchingEngineStateMachine::apply_new_order_unguarded(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                           int32_t trading_date, const fix_common::SessionIdentity& session, CommandSource source) {
    // One path, whichever way the order arrives, so that an engine that applies it live and one that applies it while
    // catching up hold the same book and send the same reports. The source makes these differences and no others: a
    // catch-up does not check the order of sequence numbers, whose records are below the position by construction; its
    // reports are marked as possible repeats, because
    // nothing in the record says whether an earlier engine reported it (R-0122); and an order it puts on the book is
    // not replicated to the follower or counted as processed.
    //
    // A catch-up reports every order it applies. Most of these records were reported by the engine that served them,
    // but an order the sequencer deferred while no engine was running was never sent to any engine, so nothing ever
    // reported it: staying silent would leave it resting on the book with its member never told it exists (BUG-0064).
    //
    // An order arriving live while no connection for reports is up is acted on all the same: it is in the sequencer's
    // WAL, and the book must agree with the WAL. Its reports are held by MatchingEngineThread until a connection is
    // established (docs/bug_list.md BUG-0120).
    const bool live = source == CommandSource::live;
    const ReportIsRepeat repeat = live ? ReportIsRepeat::no : ReportIsRepeat::yes;

    if (!live && already_applied(sequence_number)) {
        return;
    }
    if (!ReportIdentifiers::fits(sequence_number, 0)) {
        refuse_to_name(sequence_number, source);
        return;
    }
    identifiers_.use_trading_date(trading_date);
    if (live && sequence_goes_backwards(sequence_number)) {
        // Not published: the sequence number is at or below the position already published.
        refuse_out_of_sequence_order(view, sequence_number, transact_time, session);
        return;
    }
    if (transact_time == 0 || trading_date == 0) {
        refuse_order_without_time(view, sequence_number, transact_time, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }
    if (!OrderKey::fits(view.cl_ord_id)) {
        refuse_over_long_order(view, sequence_number, transact_time, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }
    const OrderKey order_key = OrderKey::make(session, view.cl_ord_id);

    // Stack-allocated ID buffers -- no heap allocation.
    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    // Refuse everything while trading is halted, and say so in the reply to the order the
    // member sent. That is how a member learns of a halt: it is the answer to the question it
    // actually asked, and it reaches the member who is trading, which is the member who needs
    // to know. A broadcast should exist as well and does not replace this -- see
    // docs/availability/design_notes.md section 15.
    //
    // During a catch-up too: the halt was set by a command at a position in the WAL, so the engine that applied this
    // order live refused it, and an engine catching up must refuse it as well to hold the same book.
    if (book_.halted()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineStateMachine: trading is halted -- refusing NOS ClOrdID={} (session comp_id='{}')", view.cl_ord_id, session.comp_id_view());

        pubsub_itc_fw_app::ExecutionReport halt_er{};
        halt_er.order_id = "NONE";
        halt_er.exec_id = exec_id;
        halt_er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        halt_er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        halt_er.symbol = view.symbol;
        halt_er.side = view.side;
        halt_er.leaves_qty = "0";
        halt_er.cum_qty = "0";
        halt_er.avg_px = "0.00";
        halt_er.transact_time = transact_time;
        halt_er.has_cl_ord_id = true;
        halt_er.cl_ord_id = view.cl_ord_id;
        halt_er.has_order_qty = true;
        halt_er.order_qty = view.order_qty;
        halt_er.has_ord_rej_reason = true;
        // The venue is closed to business rather than the order being wrong, which is what
        // this reason says and none of the others do.
        halt_er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::ExchangeClosed;
        halt_er.has_text = true;
        halt_er.text = "trading is halted";

        send_report(halt_er, sequence_number, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }

    // Reject duplicate ClOrdID within the same FIX session, whichever way the order arrives: the
    // record in the WAL is a second order with an identifier the session already has open.
    //
    // Info rather than Warning: the member is doing what R-0003 tells it to. A member that
    // does not know whether the venue took an order resubmits under the original identifier,
    // and R-0006 requires the venue to refuse it. Both sides are behaving as specified, so
    // this is not a fault of the venue's and must not be logged as one.
    if (book_.contains(order_key)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: duplicate ClOrdID={} (session comp_id='{}') -- rejecting NOS{}",
                   view.cl_ord_id, session.comp_id_view(), live ? "" : " -- during catch-up, reported as a possible repeat");
        outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::RefusedDuplicate, repeat, sequence_number, {});

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = "NONE";
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol = view.symbol;
        er.side = view.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = transact_time;
        er.has_cl_ord_id = true;
        er.cl_ord_id = view.cl_ord_id;
        er.has_order_qty = true;
        er.order_qty = view.order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::DuplicateOrder;

        send_report(er, sequence_number, session, repeat);
        publish_after_refusal(sequence_number, source);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: sent rejection ER ExecID={} ClOrdID={} (DuplicateOrder)", exec_id,
                   view.cl_ord_id);
        return;
    }

    // Accept the order: add to book and acknowledge with ExecType=New.
    ReportIdentifiers::Buffer order_id_buf{};
    const std::string_view order_id = identifiers_.order_id(order_id_buf, sequence_number);

    OrderEntry entry{};
    entry.placed_at_seq_no = sequence_number;
    entry.session = session;
    entry.side = view.side;
    entry.has_price = view.has_price;
    entry.ord_type = view.ord_type;
    entry.has_time_in_force = view.has_time_in_force;
    entry.time_in_force = view.time_in_force;
    // Stored exactly as sent -- no rounding to a session close, no trading-calendar
    // resolution, no clamp to a maximum lifetime. See OrderEntry::expire_time for why this
    // is a decision rather than merely the absence of one.
    entry.has_expire_time = view.has_expire_time;
    entry.expire_time = view.expire_time;
    entry.set_symbol(view.symbol);
    entry.set_order_qty(view.order_qty);
    if (view.has_price) {
        entry.set_price(view.price);
    }
    const bool added = book_.add(order_key, entry, sequence_number);
    book_changed_by_command_ = added;
    if (!added) {
        // The region is full, so the venue cannot record this order in a form that would
        // survive a restart, and must not accept what it cannot account for afterwards. A
        // refusal is the only one of the three answers a member can act on: growing the region
        // would fault pages in on this thread, and reusing the oldest record would silently
        // lose an order the venue had accepted. See docs/durability/open_order_checkpoint.md.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: order book region full at {} order(s) -- rejecting NOS seq={} ClOrdID={} (session comp_id='{}'){}",
                   book_.region_capacity(), sequence_number, view.cl_ord_id, session.comp_id_view(),
                   live ? "" : " -- during catch-up, reported as a possible repeat");

        pubsub_itc_fw_app::ExecutionReport full_er{};
        full_er.order_id = "NONE";
        full_er.exec_id = exec_id;
        full_er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        full_er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        full_er.symbol = view.symbol;
        full_er.side = view.side;
        full_er.leaves_qty = "0";
        full_er.cum_qty = "0";
        full_er.avg_px = "0.00";
        full_er.transact_time = transact_time;
        full_er.has_cl_ord_id = true;
        full_er.cl_ord_id = view.cl_ord_id;
        full_er.has_order_qty = true;
        full_er.order_qty = view.order_qty;
        full_er.has_ord_rej_reason = true;
        // None of the specific reasons fits: nothing is wrong with the order or the member.
        // The text says what is, because the reason code alone would leave the member looking
        // for a fault of its own that it will not find.
        full_er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
        full_er.has_text = true;
        full_er.text = "the venue is holding as many open orders as it can";

        send_report(full_er, sequence_number, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }

    if (live) {
        // Counted on the one path that puts a live order on the book. An order applied from a catch-up was counted, if
        // at all, by the engine that served it, and a refusal is not an order processed.
        outputs_.order_accepted();
        // Replicate the new entry to the follower engine.
        outputs_.order_added_to_book(sequence_number, session, view.cl_ord_id, entry);
    }

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = order_id;
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::New;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::New;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = view.order_qty;
    er.cum_qty = "0";
    er.avg_px = "0.00";
    // The time the venue took the order, not the time it got round to saying so. A member
    // reconciling its day wants when the order was accepted.
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = view.order_qty;
    er.has_ord_type = true;
    er.ord_type = view.ord_type;
    // Echoed so the gateway can tell a persistent order from a day order without holding
    // the original NOS. Cancel-on-disconnect exempts GoodTillCancel and GoodTillDate, and
    // the ER is the only thing the gateway sees for an order resting on the book.
    if (view.has_expire_time) {
        // The member's confirmation of when its order dies -- one of the terms it is
        // trading on, and the only way it can tell which expiry convention this venue
        // follows. (The FIX rendering on the wire is second-precision, matching what the
        // gateway's inbound parser reads; that is a property of the protocol hop rather
        // than an adjustment to the value.)
        er.has_expire_time = true;
        er.expire_time = view.expire_time;
    }
    if (view.has_time_in_force) {
        er.has_time_in_force = true;
        er.time_in_force = view.time_in_force;
    }
    if (view.has_price) {
        er.has_price = true;
        er.price = view.price;
    }

    // Echo the order's NoUnderlyings and NoPartyIDs groups back on the acknowledgement so
    // downstream topic subscribers see the instrument legs and parties the client sent. The
    // element arrays live in the reusable er_group_arena_buffer_ (string_views point into
    // the still-live NOS payload); the report is encoded synchronously by send_report below.
    // The arena is sized to need, not a fixed cap: echo, and if it was too small (bytes_used
    // exceeds it) grow to the requirement and retry, so large group sets are never silently
    // dropped. bytes_used reports the true need even when an allocation was refused.
    for (;;) {
        pubsub_itc_fw::BumpAllocator group_arena(er_group_arena_buffer_.data(), er_group_arena_buffer_.size());
        er.no_underlyings = {};
        er.no_party_i_ds = {};
        echo_underlyings(view.no_underlyings, group_arena, er.no_underlyings);
        echo_parties(view.no_party_i_ds, group_arena, er.no_party_i_ds);
        if (group_arena.bytes_used() <= er_group_arena_buffer_.size() || er_group_arena_buffer_.size() >= max_er_group_arena_size) {
            break;
        }
        er_group_arena_buffer_.resize(std::max(group_arena.bytes_used(), er_group_arena_buffer_.size() * 2));
    }

    // The session comes from the record rather than from the sequencer's routing map: an order that was never
    // forwarded was never entered in that map, so the identity on the envelope is the only way a catch-up's report
    // reaches the member who placed it.
    send_report(er, sequence_number, session, repeat);
    // Last, and only now. The report has left, so a death from here loses nothing: the
    // record is already in the region and this says how far the region can be trusted.
    book_.publish(sequence_number);

    // At Debug, because it is written for every order: at Info it costs 226 bytes per order, about
    // 9.5 GB a day at 40 million orders. Nothing reads it. A test learns that an order was accepted
    // from the OrderOutcome test event below, and a load run from the counter orders_processed_total.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineStateMachine: accepted NOS seq={} OrderID={} ExecID={} ClOrdID={} book_size={}{}",
               sequence_number, order_id, exec_id, view.cl_ord_id, book_.size(), live ? "" : " -- during catch-up, reported as a possible repeat");
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::Accepted, repeat, sequence_number, {});
}

void MatchingEngineStateMachine::apply_cancel_unguarded(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                                        int32_t trading_date, const fix_common::SessionIdentity& session, CommandSource source) {
    // One path, whichever way the cancel arrives, for the reasons apply_new_order gives, with the same differences. A
    // cancel the sequencer deferred was never sent to any engine either, so its member is still holding an order it
    // believes is live, and a catch-up reports it. Acted on even with no connection for reports up, as an order is.
    const bool live = source == CommandSource::live;
    const ReportIsRepeat repeat = live ? ReportIsRepeat::no : ReportIsRepeat::yes;

    if (!live && already_applied(sequence_number)) {
        return;
    }
    if (!ReportIdentifiers::fits(sequence_number, 0)) {
        refuse_to_name(sequence_number, source);
        return;
    }
    identifiers_.use_trading_date(trading_date);
    if (live && sequence_goes_backwards(sequence_number)) {
        // Not published, as for an order.
        refuse_out_of_sequence_cancel(view, sequence_number, transact_time, session);
        return;
    }
    if (transact_time == 0 || trading_date == 0) {
        refuse_cancel_without_time(view, sequence_number, transact_time, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }
    if (!OrderKey::fits(view.cl_ord_id) || !OrderKey::fits(view.orig_cl_ord_id)) {
        refuse_over_long_cancel(view, sequence_number, transact_time, session, repeat);
        publish_after_refusal(sequence_number, source);
        return;
    }
    const OrderKey orig_key = OrderKey::make(session, view.orig_cl_ord_id);

    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    // At Debug, because it is written for every cancel, as the line for each accepted order is.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineStateMachine: OrderCancelRequest seq={} ClOrdID={} OrigClOrdID={} Symbol={} Side={}{}",
               sequence_number, view.cl_ord_id, view.orig_cl_ord_id, view.symbol, static_cast<char>(view.side), live ? "" : " -- during catch-up");

    const OrderEntry* found = book_.find(orig_key);
    if (found == nullptr) {
        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = "NONE";
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol = view.symbol;
        er.side = view.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = transact_time;
        er.has_cl_ord_id = true;
        er.cl_ord_id = view.cl_ord_id;
        er.has_orig_cl_ord_id = true;
        er.orig_cl_ord_id = view.orig_cl_ord_id;
        er.has_order_qty = view.has_order_qty;
        er.order_qty = view.order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::UnknownOrder;

        send_report(er, sequence_number, session, repeat);
        publish_after_refusal(sequence_number, source);
        // Info: a member asking to cancel an order the venue does not hold is the member's doing,
        // and the venue has answered it correctly.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: sent rejection ER ExecID={} OrigClOrdID={} (UnknownOrder){}", exec_id,
                   view.orig_cl_ord_id, live ? "" : " -- during catch-up, reported as a possible repeat");
        outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::CancelRefusedUnknown, repeat, sequence_number,
                               view.orig_cl_ord_id);
        return;
    }

    // Order found -- cancel it. Copied out first, because removing it releases the record it
    // was read from and the report below is built from these terms.
    const OrderEntry entry = *found;
    book_.remove(orig_key);
    book_changed_by_command_ = true;

    if (live) {
        // Replicate the removal to the follower engine.
        outputs_.order_removed_from_book(sequence_number, session, view.orig_cl_ord_id);
    }

    ReportIdentifiers::Buffer order_id_buf{};
    const std::string_view order_id = identifiers_.order_id(order_id_buf, entry.placed_at_seq_no);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = order_id;
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
    er.symbol = entry.get_symbol();
    er.side = entry.side;
    // A report describes the order it concerns, and a cancel is no exception: a member
    // reconciling a cancel it did not initiate -- one arriving on a reconnect, say -- has
    // only the report to tell it which order this was.
    if (entry.has_time_in_force) {
        er.has_time_in_force = true;
        er.time_in_force = entry.time_in_force;
    }
    if (entry.has_expire_time) {
        er.has_expire_time = true;
        er.expire_time = entry.expire_time;
    }
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id = view.orig_cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = entry.get_order_qty();
    if (entry.has_price) {
        er.has_price = true;
        er.price = entry.get_price();
    }
    er.has_ord_type = true;
    er.ord_type = entry.ord_type;

    send_report(er, sequence_number, session, repeat);

    // Last, and only now, for the same reason as on the accept path.
    book_.publish(sequence_number);

    // At Debug, because it is written for every cancel. A test learns that a cancel was done from the
    // OrderOutcome test event below.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineStateMachine: sent cancel ER OrderID={} ExecID={} ClOrdID={} OrigClOrdID={} book_size={}{}", order_id, exec_id, view.cl_ord_id,
               view.orig_cl_ord_id, book_.size(), live ? "" : " -- during catch-up, reported as a possible repeat");
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::Cancelled, repeat, sequence_number, view.orig_cl_ord_id);
}

void MatchingEngineStateMachine::refuse_over_long_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                        const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    // Refused rather than cut short: the book's key holds max_cl_ord_id_length characters, and
    // two identifiers that differ only beyond that would otherwise become one order. Info: the
    // gateways refuse these first, so one reaching here is unusual, but it is the member's.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineStateMachine: refusing NOS seq={} -- ClOrdID is {} characters, longer than the {} the book holds (session comp_id='{}'){}",
               sequence_number, view.cl_ord_id.size(), fix_order_limits::max_cl_ord_id_length, session.comp_id_view(),
               repeat == ReportIsRepeat::yes ? " -- during catch-up, reported as a possible repeat" : "");
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::RefusedTooLong, repeat, sequence_number, {});

    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);
    std::array<char, 64> text_buf{};
    const auto text_written =
        fmt::format_to_n(text_buf.data(), text_buf.size(), "ClOrdID exceeds maximum length of {}", fix_order_limits::max_cl_ord_id_length);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = std::string_view(text_buf.data(), std::min(static_cast<size_t>(text_written.size), text_buf.size()));
    send_report(er, sequence_number, session, repeat);
}

void MatchingEngineStateMachine::refuse_over_long_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                                         const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    const bool orig_too_long = !OrderKey::fits(view.orig_cl_ord_id);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineStateMachine: refusing OCR seq={} -- {} is {} characters, longer than the {} the book holds (session comp_id='{}'){}",
               sequence_number, orig_too_long ? "OrigClOrdID" : "ClOrdID", orig_too_long ? view.orig_cl_ord_id.size() : view.cl_ord_id.size(),
               fix_order_limits::max_cl_ord_id_length, session.comp_id_view(),
               repeat == ReportIsRepeat::yes ? " -- during catch-up, reported as a possible repeat" : "");
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::CancelRefusedTooLong, repeat, sequence_number, view.orig_cl_ord_id);

    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);
    std::array<char, 64> text_buf{};
    const auto text_written = fmt::format_to_n(text_buf.data(), text_buf.size(), "{} exceeds maximum length of {}", orig_too_long ? "OrigClOrdID" : "ClOrdID",
                                               fix_order_limits::max_cl_ord_id_length);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    // Carried so the gateways recognise the report as a refused cancel, and send it to the member
    // as an OrderCancelReject rather than as a rejected order.
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id = view.orig_cl_ord_id;
    er.has_order_qty = view.has_order_qty;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = std::string_view(text_buf.data(), std::min(static_cast<size_t>(text_written.size), text_buf.size()));
    send_report(er, sequence_number, session, repeat);
}

bool MatchingEngineStateMachine::already_applied(int64_t sequence_number) const {
    // A catch-up can deliver records this engine has already acted on live: one asked for again, after an order was
    // discarded while this instance held no lease, starts from the position the region published, which can be below
    // the last command acted on. Such a record is not applied twice. It produces no report, because the member was
    // answered when it was acted on, and does not move the published position, which is already at or above it.
    if (sequence_number > highest_live_seq_no_) {
        return false;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineStateMachine: catch-up record seq={} is at or below {}, the highest already acted on -- not applied again", sequence_number,
               highest_live_seq_no_);
    return true;
}

bool MatchingEngineStateMachine::sequence_goes_backwards(int64_t sequence_number) {
    if (sequence_number <= highest_live_seq_no_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: a command arrived with sequence number {}, not above {}, the highest this engine has already acted on -- "
                   "refusing it, because acting on it would leave the book disagreeing with the leading sequencer's WAL",
                   sequence_number, highest_live_seq_no_);
        return true;
    }
    highest_live_seq_no_ = sequence_number;
    return false;
}

void MatchingEngineStateMachine::refuse_out_of_sequence_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                              const fix_common::SessionIdentity& session) {
    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = "Venue could not process the order: it arrived out of sequence";
    send_report(er, sequence_number, session, ReportIsRepeat::no);
}

void MatchingEngineStateMachine::refuse_out_of_sequence_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number,
                                                               int64_t transact_time, const fix_common::SessionIdentity& session) {
    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    // Carried so the gateways send the report to the member as an OrderCancelReject.
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id = view.orig_cl_ord_id;
    er.has_order_qty = view.has_order_qty;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = "Venue could not process the cancel: it arrived out of sequence. The order is unchanged";
    send_report(er, sequence_number, session, ReportIsRepeat::no);
}

void MatchingEngineStateMachine::refuse_order_without_time(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                           const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    // The sequencer writes the time it took every command, and the trading date, into the command's WAL
    // record, so a command without either is a defect in the sequencer. The engine does not supply one of
    // its own, which would differ from engine to engine and from one replay to the next.
    const std::string_view missing = transact_time == 0 ? "time" : "trading date";
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineStateMachine: refusing NOS seq={} ClOrdID={} (session comp_id='{}') -- its WAL record carries no {}, which the sequencer "
               "always writes, so the sequencer is at fault{}",
               sequence_number, view.cl_ord_id, session.comp_id_view(), missing,
               repeat == ReportIsRepeat::yes ? " -- during catch-up, reported as a possible repeat" : "");
    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    // What the command carried, zero where it carried no time: the same on every engine and every replay.
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = transact_time == 0 ? "Venue could not process the order: it carried no time" : "Venue could not process the order: it carried no trading date";
    send_report(er, sequence_number, session, repeat);
}

void MatchingEngineStateMachine::refuse_cancel_without_time(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number,
                                                            int64_t transact_time, const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    const std::string_view missing = transact_time == 0 ? "time" : "trading date";
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineStateMachine: refusing OCR seq={} OrigClOrdID={} (session comp_id='{}') -- its WAL record carries no {}, which the "
               "sequencer always writes, so the sequencer is at fault{}",
               sequence_number, view.orig_cl_ord_id, session.comp_id_view(), missing,
               repeat == ReportIsRepeat::yes ? " -- during catch-up, reported as a possible repeat" : "");
    ReportIdentifiers::Buffer exec_id_buf{};
    const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, 0);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = "NONE";
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = transact_time;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    // Carried so the gateways send the report to the member as an OrderCancelReject.
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id = view.orig_cl_ord_id;
    er.has_order_qty = view.has_order_qty;
    er.order_qty = view.order_qty;
    er.has_ord_rej_reason = true;
    er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::Other;
    er.has_text = true;
    er.text = transact_time == 0 ? "Venue could not process the cancel: it carried no time. The order is unchanged"
                                 : "Venue could not process the cancel: it carried no trading date. The order is unchanged";
    send_report(er, sequence_number, session, repeat);
}

void MatchingEngineStateMachine::set_halted(TradingHalted halted, int64_t sequence_number, CommandSource source) {
    const bool now_halted = halted == TradingHalted::yes;
    if (now_halted == book_.halted()) {
        return;
    }
    book_.set_halted(halted);
    if (source == CommandSource::live) {
        outputs_.trading_halt_changed(sequence_number, now_halted);
    }
}

void MatchingEngineStateMachine::refuse_to_name(int64_t sequence_number, CommandSource source) {
    // An identifier holds sequence numbers below 2^36, about 68,700 million, far above any day's WAL. A command beyond it
    // cannot be given a report whose identifier is not also some other report's, so it is not answered at all, and
    // trading halts so that nothing more is accepted that could not be named.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineStateMachine: command seq={} is beyond the {} sequence numbers an identifier can hold -- not applied and not answered, "
               "and trading halts",
               sequence_number, ReportIdentifiers::sequence_number_limit);
    set_halted(TradingHalted::yes, sequence_number, source);
}

void MatchingEngineStateMachine::publish_after_refusal(int64_t sequence_number, CommandSource source) {
    // Live, the book is published only when a command has changed it: a refusal changes nothing, and is left above the
    // published position, so that an engine restarting applies it again and refuses it again, marked as a possible
    // repeat. During a catch-up the book is published after every command, so that the position the catch-up reached
    // is the position the region records. Where the published position stands is not part of what the engine reports.
    if (source == CommandSource::catch_up) {
        book_.publish(sequence_number);
    }
}

void MatchingEngineStateMachine::after_failure(int64_t sequence_number, CommandSource source, const char* what,
                                               const std::function<void(ReportIdentifiers::Buffer&)>& send_rejection) {
    // A failure while applying one command costs that command and not the engine: a C++ exception or a failed check of
    // the engine's own state, thrown by anything the command reached. Both engines of a pair, being deterministic, meet
    // it at the same command and answer it alike.
    //
    // The command is rejected to its member only when it had changed nothing and answered nothing: the checks run before
    // the book is changed, so a failure there leaves the book as it was. Where the book was changed, or a report was
    // sent, the command took effect as far as the book or the member is concerned, and a rejection would contradict it,
    // so it is only reported here.
    const bool untouched = !book_changed_by_command_ && reports_for_command_ == 0;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "MatchingEngineStateMachine: command seq={} failed while it was applied: {} -- {}", sequence_number,
               what,
               untouched ? "it changed nothing, and is rejected to its member"
                         : "it had already changed the book or been answered, so it stands as far as it went, and nothing more is sent");
    try {
        if (untouched) {
            ReportIdentifiers::Buffer exec_id_buf{};
            send_rejection(exec_id_buf);
            publish_after_refusal(sequence_number, source);
        } else if (book_changed_by_command_) {
            book_.publish(sequence_number);
        }
    } catch (const std::exception& second) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: command seq={} could not be rejected either: {} -- its member is not answered", sequence_number, second.what());
    }
}

MatchingEngineStateMachine::BookCancelledByCommand MatchingEngineStateMachine::apply_cancel_book(int64_t orders_placed_through,
                                                                                                 HaltIfOrdersCancelled halt_if_orders_cancelled,
                                                                                                 int64_t sequence_number, int64_t transact_time,
                                                                                                 int32_t trading_date, CommandSource source) {
    BookCancelledByCommand result;
    const bool live = source == CommandSource::live;
    if (!live && already_applied(sequence_number)) {
        return result;
    }
    if (!ReportIdentifiers::fits(sequence_number, 0)) {
        refuse_to_name(sequence_number, source);
        return result;
    }
    if (live && sequence_goes_backwards(sequence_number)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: CancelBook seq={} is at or below the highest command acted on -- not applied, because the WAL this "
                   "engine acted on and the one now sending disagree",
                   sequence_number);
        return result;
    }
    if (transact_time == 0 || trading_date == 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: CancelBook seq={} carries no {} -- a defect in the sequencer; applied with what it carries, because it has "
                   "no member to refuse it to",
                   sequence_number, transact_time == 0 ? "time" : "trading date");
    }
    identifiers_.use_trading_date(trading_date);
    const ReportIsRepeat repeat = live ? ReportIsRepeat::no : ReportIsRepeat::yes;
    result.applied = true;
    result.open_orders = static_cast<int64_t>(book_.size());
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineStateMachine: CancelBook seq={} -- cancelling the orders placed at or before seq={}, of {} on the book{}", sequence_number,
               orders_placed_through, book_.size(), live ? "" : " -- during catch-up, reported as possible repeats");

    int64_t place = 0;
    result.reports_sent = static_cast<int64_t>(book_.remove_placed_through(orders_placed_through, [&](const OrderEntry& entry) {
        ReportIdentifiers::Buffer exec_id_buf{};
        const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, place++);
        ReportIdentifiers::Buffer order_id_buf{};
        const std::string_view order_id = identifiers_.order_id(order_id_buf, entry.placed_at_seq_no);

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = order_id;
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
        er.symbol = entry.get_symbol();
        er.side = entry.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = transact_time;
        er.has_cl_ord_id = true;
        er.cl_ord_id = entry.get_cl_ord_id();
        er.has_order_qty = true;
        er.order_qty = entry.get_order_qty();
        if (entry.has_price) {
            er.has_price = true;
            er.price = entry.get_price();
        }
        er.has_ord_type = true;
        er.ord_type = entry.ord_type;
        if (entry.has_time_in_force) {
            er.has_time_in_force = true;
            er.time_in_force = entry.time_in_force;
        }
        if (entry.has_expire_time) {
            er.has_expire_time = true;
            er.expire_time = entry.expire_time;
        }
        // Sequence number 0: the sequencer routes a report by the sequence number of the order it answers, and this
        // command was placed by no member, so the session that placed the order rides on the report's envelope instead.
        outputs_.send_report(er, 0, entry.session, repeat);
        if (live) {
            outputs_.order_removed_from_book(sequence_number, entry.session, entry.get_cl_ord_id());
        }
    }));
    result.orders_kept = static_cast<int64_t>(book_.size());
    // Recorded before the position is published, so that an engine that restarts after this command finds the halt.
    if (halt_if_orders_cancelled == HaltIfOrdersCancelled::yes && result.reports_sent > 0 && !book_.halted()) {
        set_halted(TradingHalted::yes, sequence_number, source);
        result.halted = true;
    }
    book_.publish(sequence_number);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineStateMachine: CancelBook seq={} -- {} order(s) cancelled and reported, {} placed after seq={} kept{}", sequence_number,
               result.reports_sent, result.orders_kept, orders_placed_through, result.halted ? ", and trading halts" : "");
    return result;
}

MatchingEngineStateMachine::TechnicalEventApplied MatchingEngineStateMachine::apply_technical_event(const pubsub_itc_fw_app::TechnicalEventView& view,
                                                                                                    int64_t sequence_number, int32_t trading_date,
                                                                                                    CommandSource source) {
    TechnicalEventApplied result;
    const bool live = source == CommandSource::live;
    if (!live && already_applied(sequence_number)) {
        return result;
    }
    if (live && sequence_goes_backwards(sequence_number)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: TechnicalEvent seq={} is at or below the highest command acted on -- not applied, because the WAL this "
                   "engine acted on and the one now sending disagree",
                   sequence_number);
        return result;
    }
    if (view.trading_day < trading_date) {
        // Published all the same, so that the position moves past it and a restart does not meet it again as new.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: TechnicalEvent seq={} is for trading day {}, older than the trading day {} of the record carrying it -- "
                   "refused, so that it cannot change trading on a later day",
                   sequence_number, view.trading_day, trading_date);
        book_.publish(sequence_number);
        return result;
    }
    result.applied = true;
    result.halt_changed = view.halted != book_.halted();
    set_halted(view.halted ? TradingHalted::yes : TradingHalted::no, sequence_number, source);
    book_.publish(sequence_number);
    PUBSUB_LOG(logger_, view.halted ? pubsub_itc_fw::FwLogLevel::Error : pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineStateMachine: TechnicalEvent seq={} -- phase {}, trading day {}, {} ({}){}", sequence_number,
               pubsub_itc_fw_app::to_string(view.phase), view.trading_day, view.halted ? "trading is halted" : "trading is not halted",
               pubsub_itc_fw_app::to_string(view.reason), live ? "" : " -- during catch-up");
    return result;
}

MatchingEngineStateMachine::BookCancelled MatchingEngineStateMachine::cancel_every_order(int64_t sequence_number, int64_t first_place, int64_t transact_time,
                                                                                         int32_t trading_date) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: cancelling every order -- {} live order(s)", book_.size());
    BookCancelled result;
    result.open_orders = static_cast<int64_t>(book_.size());
    identifiers_.use_trading_date(trading_date);

    // In the order the orders were placed, which every engine that applied the same commands agrees on, so that two
    // engines cancelling the same book number and send the same reports in the same order.
    int64_t place = first_place;
    book_.for_each_in_order_placed([&](const OrderEntry& entry) {
        ReportIdentifiers::Buffer exec_id_buf{};
        const std::string_view exec_id = identifiers_.exec_id(exec_id_buf, sequence_number, place++);
        ReportIdentifiers::Buffer order_id_buf{};
        const std::string_view order_id = identifiers_.order_id(order_id_buf, entry.placed_at_seq_no);
        const std::string_view cl_ord_id = entry.get_cl_ord_id();

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = order_id;
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
        er.symbol = entry.get_symbol();
        er.side = entry.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = transact_time;
        er.has_cl_ord_id = true;
        er.cl_ord_id = cl_ord_id;
        er.has_order_qty = true;
        er.order_qty = entry.get_order_qty();
        if (entry.has_price) {
            er.has_price = true;
            er.price = entry.get_price();
        }
        er.has_ord_type = true;
        er.ord_type = entry.ord_type;

        // Sequence number 0: no sequenced command produced these cancels, so the sequencer cannot
        // route them by the sequence number of the order. The session that placed the order rides
        // on the report's envelope instead.
        outputs_.send_report(er, 0, entry.session, ReportIsRepeat::no);
        ++result.reports_sent;
    });

    book_.clear();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: cancelled every order -- {} cancel ER(s) sent, book cleared",
               result.reports_sent);
    return result;
}

} // namespaces
