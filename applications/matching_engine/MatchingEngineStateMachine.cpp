// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineStateMachine.hpp"

#include <algorithm>

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

MatchingEngineStateMachine::MatchingEngineStateMachine(OrderBook& book, MatchingEngineOutputsInterface& outputs, pubsub_itc_fw::QuillLogger& logger)
    : book_(book), outputs_(outputs), logger_(logger) {}

void MatchingEngineStateMachine::note_recovered_orders(int64_t highest_order_id_num) {
    order_id_counter_ = std::max(order_id_counter_, highest_order_id_num);
}

void MatchingEngineStateMachine::note_caught_up_to(int64_t sequence_number) {
    highest_live_seq_no_ = std::max(highest_live_seq_no_, sequence_number);
}

void MatchingEngineStateMachine::apply_new_order_from_catch_up(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number,
                                                               int64_t transact_time, const fix_common::SessionIdentity& session) {
    // Every order applied from a catch-up is reported, and every report is marked as a possible
    // repeat. Most of these records were reported by the engine that served them, but an order the
    // sequencer deferred while no engine was running was never sent to any engine, so nothing ever
    // reported it: staying silent here would leave it resting on the book with its member never told
    // it exists (BUG-0064).
    //
    // Nothing here can tell the two apart. The engine sees a record; whether some earlier engine
    // reported it is not in the record. So every one is reported and every one is marked as a
    // possible repeat, which is R-0122: a member that already had the report can discard it, and a
    // member that never had one is answered. Silence is the only outcome that cannot be recovered
    // from.
    if (!OrderKey::fits(view.cl_ord_id)) {
        refuse_over_long_order(view, sequence_number, transact_time, session, ReportIsRepeat::yes);
        book_.publish(sequence_number);
        return;
    }
    const OrderKey recon_key = OrderKey::make(session, view.cl_ord_id);
    if (book_.contains(recon_key)) {
        return; // duplicate during replay -- ignore
    }
    OrderEntry recon_entry{};
    recon_entry.order_id_num = ++order_id_counter_;
    recon_entry.side = view.side;
    recon_entry.has_price = view.has_price;
    recon_entry.ord_type = view.ord_type;
    recon_entry.set_symbol(view.symbol);
    recon_entry.set_order_qty(view.order_qty);
    if (view.has_price) {
        recon_entry.set_price(view.price);
    }
    recon_entry.session = session;
    if (!book_.add(recon_key, recon_entry, sequence_number)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: catch-up cannot apply NOS seq={} cl_ord_id={} -- the order book region is full at {} order(s)", sequence_number,
                   view.cl_ord_id, book_.region_capacity());
        return;
    }
    std::array<char, 32> recon_exec_id_buf{};
    const std::string_view recon_exec_id = format_id(recon_exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);
    std::array<char, 32> recon_order_id_buf{};
    const std::string_view recon_order_id = format_id(recon_order_id_buf, "ME-ORD-", 7, recon_entry.order_id_num);

    pubsub_itc_fw_app::ExecutionReport recon_er{};
    recon_er.order_id = recon_order_id;
    recon_er.exec_id = recon_exec_id;
    recon_er.exec_type = pubsub_itc_fw_app::ExecType::New;
    recon_er.ord_status = pubsub_itc_fw_app::OrdStatus::New;
    recon_er.symbol = view.symbol;
    recon_er.side = view.side;
    recon_er.leaves_qty = view.order_qty;
    recon_er.cum_qty = "0";
    recon_er.avg_px = "0.00";
    // The time the venue took the order, not the time it got round to saying so. A member
    // reconciling its day wants when the order was accepted.
    recon_er.transact_time = transact_time;
    recon_er.has_cl_ord_id = true;
    recon_er.cl_ord_id = view.cl_ord_id;
    recon_er.has_order_qty = true;
    recon_er.order_qty = view.order_qty;
    if (view.has_price) {
        recon_er.has_price = true;
        recon_er.price = view.price;
    }
    recon_er.has_ord_type = true;
    recon_er.ord_type = view.ord_type;

    // The session comes from the record rather than from the sequencer's routing map: an
    // order that was never forwarded was never entered in that map, so the identity on
    // the envelope is the only way this report reaches the member who placed it.
    outputs_.send_report(recon_er, sequence_number, session, ReportIsRepeat::yes);

    // Published only after the report is away, which is the ordering the accept path uses
    // and for the same reason: a death between the two must leave the order above the
    // published position, so recovery discards it and the record re-runs.
    book_.publish(sequence_number);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineStateMachine: catch-up applied NOS seq={} cl_ord_id={} book_size={} -- reported as a possible repeat", sequence_number,
               view.cl_ord_id, book_.size());
}

void MatchingEngineStateMachine::apply_new_order(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t transact_time,
                                                 const fix_common::SessionIdentity& session, CommandSource source) {
    if (source == CommandSource::catch_up) {
        apply_new_order_from_catch_up(view, sequence_number, transact_time, session);
        return;
    }

    // An order arriving while no connection for reports is up is acted on all the same: it is in the
    // sequencer's WAL, and the book must agree with the WAL. Its reports are held by
    // MatchingEngineThread until a connection is established (docs/bug_list.md BUG-0120).

    if (sequence_goes_backwards(sequence_number)) {
        refuse_out_of_sequence_order(view, sequence_number, transact_time, session);
        return;
    }
    if (!OrderKey::fits(view.cl_ord_id)) {
        refuse_over_long_order(view, sequence_number, transact_time, session, ReportIsRepeat::no);
        return;
    }
    const OrderKey order_key = OrderKey::make(session, view.cl_ord_id);

    // Stack-allocated ID buffers -- no heap allocation.
    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

    // Refuse everything while trading is halted, and say so in the reply to the order the
    // member sent. That is how a member learns of a halt: it is the answer to the question it
    // actually asked, and it reaches the member who is trading, which is the member who needs
    // to know. A broadcast should exist as well and does not replace this -- see
    // docs/availability/design_notes.md section 15.
    if (halted_) {
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

        outputs_.send_report(halt_er, sequence_number, session, ReportIsRepeat::no);
        return;
    }

    // Reject duplicate ClOrdID within the same FIX session.
    //
    // Info rather than Warning: the member is doing what R-0003 tells it to. A member that
    // does not know whether the venue took an order resubmits under the original identifier,
    // and R-0006 requires the venue to refuse it. Both sides are behaving as specified, so
    // this is not a fault of the venue's and must not be logged as one.
    if (book_.contains(order_key)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: duplicate ClOrdID={} (session comp_id='{}') -- rejecting NOS",
                   view.cl_ord_id, session.comp_id_view());
        outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::RefusedDuplicate, ReportIsRepeat::no, sequence_number, {});

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

        outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: sent rejection ER ExecID={} ClOrdID={} (DuplicateOrder)", exec_id,
                   view.cl_ord_id);
        return;
    }

    // Accept the order: add to book and acknowledge with ExecType=New.
    std::array<char, 32> order_id_buf{};
    const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, ++order_id_counter_);

    OrderEntry entry{};
    entry.order_id_num = order_id_counter_;
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
    if (!book_.add(order_key, entry, sequence_number)) {
        // The region is full, so the venue cannot record this order in a form that would
        // survive a restart, and must not accept what it cannot account for afterwards. A
        // refusal is the only one of the three answers a member can act on: growing the region
        // would fault pages in on this thread, and reusing the oldest record would silently
        // lose an order the venue had accepted. See docs/durability/open_order_checkpoint.md.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineStateMachine: order book region full at {} order(s) -- rejecting NOS ClOrdID={} (session comp_id='{}')",
                   book_.region_capacity(), view.cl_ord_id, session.comp_id_view());

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

        outputs_.send_report(full_er, sequence_number, session, ReportIsRepeat::no);
        return;
    }

    // Counted here, on the one path that puts a live order on the book. The catch-up path and the
    // duplicate-ClOrdID rejection above are not orders processed.
    outputs_.order_accepted();

    // Replicate the new entry to the follower engine.
    outputs_.order_added_to_book(sequence_number, session, view.cl_ord_id, entry);

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

    outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);
    // Last, and only now. The report has left, so a death from here loses nothing: the
    // record is already in the region and this says how far the region can be trusted.
    book_.publish(sequence_number);

    // At Debug, because it is written for every order: at Info it costs 226 bytes per order, about
    // 9.5 GB a day at 40 million orders. Nothing reads it. A test learns that an order was accepted
    // from the OrderOutcome test event below, and a load run from the counter orders_processed_total.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineStateMachine: accepted NOS OrderID={} ExecID={} ClOrdID={} book_size={}", order_id,
               exec_id, view.cl_ord_id, book_.size());
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::Accepted, ReportIsRepeat::no, sequence_number, {});
}

void MatchingEngineStateMachine::apply_cancel_from_catch_up(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number,
                                                            int64_t transact_time, const fix_common::SessionIdentity& session) {
    // Applied and reported, for the reason apply_new_order_from_catch_up gives. A cancel the
    // sequencer deferred was never sent to any engine either, so its member is still holding an
    // order it believes is live.
    if (!OrderKey::fits(view.cl_ord_id) || !OrderKey::fits(view.orig_cl_ord_id)) {
        refuse_over_long_cancel(view, sequence_number, transact_time, session, ReportIsRepeat::yes);
        book_.publish(sequence_number);
        return;
    }
    const OrderKey recon_key = OrderKey::make(session, view.orig_cl_ord_id);
    // Read before the removal, because the report names the order that was cancelled and
    // the entry is what holds its venue order id.
    const OrderEntry* recon_cancelled = book_.find(recon_key);
    std::array<char, 32> recon_order_id_buf{};
    const std::string_view recon_order_id =
        recon_cancelled != nullptr ? format_id(recon_order_id_buf, "ME-ORD-", 7, recon_cancelled->order_id_num) : std::string_view{"NONE"};
    const bool recon_was_open = recon_cancelled != nullptr;
    book_.remove(recon_key);

    if (recon_was_open) {
        std::array<char, 32> recon_exec_id_buf{};
        const std::string_view recon_exec_id = format_id(recon_exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

        pubsub_itc_fw_app::ExecutionReport recon_er{};
        recon_er.order_id = recon_order_id;
        recon_er.exec_id = recon_exec_id;
        recon_er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
        recon_er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
        recon_er.symbol = view.symbol;
        recon_er.side = view.side;
        recon_er.leaves_qty = "0";
        recon_er.cum_qty = "0";
        recon_er.avg_px = "0.00";
        recon_er.transact_time = transact_time;
        recon_er.has_cl_ord_id = true;
        recon_er.cl_ord_id = view.cl_ord_id;
        recon_er.has_orig_cl_ord_id = true;
        recon_er.orig_cl_ord_id = view.orig_cl_ord_id;
        // OrderQty is optional on a cancel (R-0142), so the report carries it only if the cancel did.
        recon_er.has_order_qty = view.has_order_qty;
        recon_er.order_qty = view.order_qty;
        outputs_.send_report(recon_er, sequence_number, session, ReportIsRepeat::yes);
    }

    book_.publish(sequence_number);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineStateMachine: catch-up applied OCR seq={} orig_cl_ord_id={} book_size={} (was open: {})", sequence_number, view.orig_cl_ord_id,
               book_.size(), recon_was_open);
}

void MatchingEngineStateMachine::apply_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t transact_time,
                                              const fix_common::SessionIdentity& session, CommandSource source) {
    if (source == CommandSource::catch_up) {
        apply_cancel_from_catch_up(view, sequence_number, transact_time, session);
        return;
    }

    // Acted on even with no connection for reports up, as an order is; see apply_new_order.

    if (sequence_goes_backwards(sequence_number)) {
        refuse_out_of_sequence_cancel(view, sequence_number, transact_time, session);
        return;
    }
    if (!OrderKey::fits(view.cl_ord_id) || !OrderKey::fits(view.orig_cl_ord_id)) {
        refuse_over_long_cancel(view, sequence_number, transact_time, session, ReportIsRepeat::no);
        return;
    }
    const OrderKey orig_key = OrderKey::make(session, view.orig_cl_ord_id);

    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

    // At Debug, because it is written for every cancel, as the line for each accepted order is.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineStateMachine: OrderCancelRequest seq={} ClOrdID={} OrigClOrdID={} Symbol={} Side={}",
               sequence_number, view.cl_ord_id, view.orig_cl_ord_id, view.symbol, static_cast<char>(view.side));

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

        outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);
        // Info: a member asking to cancel an order the venue does not hold is the member's doing,
        // and the venue has answered it correctly.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: sent rejection ER ExecID={} OrigClOrdID={} (UnknownOrder)", exec_id,
                   view.orig_cl_ord_id);
        outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::CancelRefusedUnknown, ReportIsRepeat::no, sequence_number,
                               view.orig_cl_ord_id);
        return;
    }

    // Order found -- cancel it. Copied out first, because removing it releases the record it
    // was read from and the report below is built from these terms.
    const OrderEntry entry = *found;
    book_.remove(orig_key);

    // Replicate the removal to the follower engine.
    outputs_.order_removed_from_book(sequence_number, session, view.orig_cl_ord_id);

    // Format order_id from the stored counter value.
    std::array<char, 32> order_id_buf{};
    const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, entry.order_id_num);

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

    outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);

    // Last, and only now, for the same reason as on the accept path.
    book_.publish(sequence_number);

    // At Debug, because it is written for every cancel. A test learns that a cancel was done from the
    // OrderOutcome test event below.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineStateMachine: sent cancel ER OrderID={} ExecID={} ClOrdID={} OrigClOrdID={} book_size={}", order_id, exec_id, view.cl_ord_id,
               view.orig_cl_ord_id, book_.size());
    outputs_.order_outcome(session, view.cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind::Cancelled, ReportIsRepeat::no, sequence_number, view.orig_cl_ord_id);
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

    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);
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
    outputs_.send_report(er, sequence_number, session, repeat);
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

    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);
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
    outputs_.send_report(er, sequence_number, session, repeat);
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
    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

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
    outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);
}

void MatchingEngineStateMachine::refuse_out_of_sequence_cancel(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number,
                                                               int64_t transact_time, const fix_common::SessionIdentity& session) {
    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

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
    outputs_.send_report(er, sequence_number, session, ReportIsRepeat::no);
}

MatchingEngineStateMachine::BookCancelled MatchingEngineStateMachine::cancel_every_order(int64_t transact_time) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineStateMachine: cancelling every order -- {} live order(s)", book_.size());
    BookCancelled result;
    result.open_orders = static_cast<int64_t>(book_.size());

    book_.for_each([&](const OrderKey& key, const OrderEntry& entry) {
        std::array<char, 32> exec_id_buf{};
        const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);
        std::array<char, 32> order_id_buf{};
        const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, entry.order_id_num);
        const std::string_view cl_ord_id{key.cl_ord_id.data(), key.cl_ord_id_len};

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
