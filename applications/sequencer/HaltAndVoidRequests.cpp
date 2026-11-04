// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "HaltAndVoidRequests.hpp"

#include <algorithm>
#include <limits>

#include <fmt/format.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

HaltAndVoidRequests::HaltAndVoidRequests(HaltAndVoidRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

pubsub_itc_fw_app::TechnicalEvent HaltAndVoidRequests::technical_event(Trading trading, pubsub_itc_fw_app::TechnicalEventReason reason, int64_t now_ns) {
    pubsub_itc_fw_app::TechnicalEvent event{};
    event.phase = pubsub_itc_fw_app::TradingPhase::Open;
    event.halted = trading == Trading::halts;
    event.trading_day = links_.trading_day(now_ns);
    event.reason = reason;
    return event;
}

// ---- An operator's request ------------------------------------------------------------------------------------------

void HaltAndVoidRequests::operator_request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, bool halt, int64_t now_ns) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "HaltAndVoidRequests: operator {} asks for trading to be {}, on connection {}", operator_name,
               halt ? "halted" : "no longer halted", connection.get_value());
    if (!links_.leading()) {
        answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::NotLeader, 0,
               "this sequencer does not lead, so it wrote nothing; the sequencer that leads acts on the request");
        return;
    }
    const TradingHaltState& trading_halt = links_.trading_halt();
    if (halt) {
        if (trading_halt.halted()) {
            answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::AlreadyHalted, 0,
                   fmt::format("trading is halted already, by the TechnicalEvent at seq={} ({}), so nothing was written", trading_halt.latest()->seq_no,
                               pubsub_itc_fw_app::to_string(trading_halt.latest()->reason)));
            return;
        }
        // Nothing is cancelled: an operator's halt keeps the book, and the operator's reasons are not the venue's to guess.
        const pubsub_itc_fw_app::TechnicalEvent event = technical_event(Trading::halts, pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator, now_ns);
        const int64_t seq_no = links_.next_seq_no();
        if (!links_.write_technical_event(event)) {
            answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::NotHalted, 0,
                   "the TechnicalEvent could not be encoded, so nothing was written");
            return;
        }
        answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::Halted, seq_no,
               fmt::format("wrote a TechnicalEvent at seq={} that halts trading on trading day {}: new orders are refused, cancels are carried out, and no "
                           "order is cancelled",
                           seq_no, event.trading_day));
        return;
    }

    switch (trading_halt.may_lift(links_.an_engine_takes_orders(), links_.highest_command_answered())) {
        case TradingHaltState::Lift::NotHalted:
            answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::NotHalted, 0, "trading is not halted, so there is nothing to lift");
            return;
        case TradingHaltState::Lift::NoEngineLeads:
            answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::NoEngineLeads, 0,
                   fmt::format("no matching engine leads, so the halt at seq={} is not lifted: start a matching engine, wait for it to lead, and ask again",
                               trading_halt.latest()->seq_no));
            return;
        case TradingHaltState::Lift::ReportsOutstanding:
            // The leading engine has not yet answered every order and cancel written before the halt, so some member has not
            // yet been sent its rejections and its cancels: the engine sends every report of a command before it applies the
            // next.
            answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::ReportsOutstanding, 0,
                   fmt::format("the WAL holds no report yet answering seq={}, the last order or cancel before the halt at seq={} (the latest command "
                               "answered is seq={}), so the halt is not lifted: the leading engine is still working through the orders the venue held. Ask "
                               "again shortly",
                               trading_halt.latest()->last_members_command_before, trading_halt.latest()->seq_no, links_.highest_command_answered()));
            return;
        case TradingHaltState::Lift::Allowed:
            break;
    }
    const TradingHaltState::TechnicalEventRecord halted_by = *trading_halt.latest();
    const pubsub_itc_fw_app::TechnicalEvent event = technical_event(Trading::lifts, pubsub_itc_fw_app::TechnicalEventReason::LiftedByOperator, now_ns);
    const int64_t seq_no = links_.next_seq_no();
    if (!links_.write_technical_event(event)) {
        answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::ReportsOutstanding, 0,
               "the TechnicalEvent could not be encoded, so nothing was written");
        return;
    }
    answer(connection, operator_name, pubsub_itc_fw_app::OperatorHaltOutcome::Lifted, seq_no,
           fmt::format("wrote a TechnicalEvent at seq={} that lifts the halt at seq={} ({}): new orders are accepted again", seq_no, halted_by.seq_no,
                       pubsub_itc_fw_app::to_string(halted_by.reason)));
}

void HaltAndVoidRequests::answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::OperatorHaltOutcome outcome,
                                 int64_t technical_event_seq_no, const std::string& explanation) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "HaltAndVoidRequests: answered operator {}: {} -- {}", operator_name,
               pubsub_itc_fw_app::to_string(outcome), explanation);
    links_.answer_operator(connection, outcome, technical_event_seq_no, explanation);
}

// ---- A matching engine's request to halt ----------------------------------------------------------------------------

void HaltAndVoidRequests::engine_halt_request(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int32_t epoch,
                                              pubsub_itc_fw_app::TechnicalEventReason reason, int64_t now_ns) {
    // As for a cancel of the book: only the leader writes commands, and only for the engine orders are routed to.
    const bool leading = links_.leading();
    if (!leading || !links_.orders_routed_to(connection)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "HaltAndVoidRequests: HaltRequest from matching engine instance {} on connection {} -- not written: {}", instance_id, connection.get_value(),
                   !leading ? "this instance does not lead" : "orders are not routed to that engine");
        return;
    }
    const pubsub_itc_fw_app::TechnicalEvent event = technical_event(Trading::halts, reason, now_ns);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "HaltAndVoidRequests: matching engine instance {} at epoch {} cannot account for the orders the venue holds -- writing a TechnicalEvent that "
               "halts trading on trading day {} ({}); a person must lift it (R-0123, R-0023)",
               instance_id, epoch, event.trading_day, pubsub_itc_fw_app::to_string(reason));
    static_cast<void>(links_.write_technical_event(event));
}

// ---- A matching engine's request to void a command ------------------------------------------------------------------

void HaltAndVoidRequests::void_request(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int64_t seq_no) {
    if (!links_.leading()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "HaltAndVoidRequests: VoidRequest for command seq={} from matching engine instance {} -- not acted on: this instance does not lead", seq_no,
                   instance_id);
        return;
    }
    // Both engines usually die on the same command, so both ask. The first request writes the void; the second is sent
    // the voids, which name the command once its record is committed, and is asked again until then.
    if (links_.voided_already(seq_no)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "HaltAndVoidRequests: VoidRequest for command seq={} from matching engine instance {} -- voided already; sending the voids", seq_no,
                   instance_id);
        links_.send_voids(connection);
        return;
    }
    // An engine that leads has caught up past the command, so it applied it, and its reports of it may not be in the WAL
    // yet: one that took the lead from following sends the reports it kept only as it takes the lead.
    if (links_.another_engine_leads(instance_id)) {
        decline(connection, instance_id, seq_no, pubsub_itc_fw_app::VoidDeclinedReason::AnotherEngineLeads,
                "the other matching engine leads, so it applied the command and lived");
        return;
    }
    if (seq_no <= links_.highest_command_answered()) {
        decline(connection, instance_id, seq_no, pubsub_itc_fw_app::VoidDeclinedReason::AnsweredAlready,
                "the WAL holds a report answering it or a later command, so an engine applied it and went on, and its member may have been told");
        return;
    }
    bool found = false;
    const CommandInRecord command = links_.command_in_wal_at(seq_no, found);
    if (!found) {
        decline(connection, instance_id, seq_no, pubsub_itc_fw_app::VoidDeclinedReason::NotInWal, "the WAL does not hold it");
        return;
    }
    if (command != CommandInRecord::MembersCommand) {
        // A command the sequencer wrote itself is not voided: that would leave open orders the venue meant to cancel, or
        // trading open that it meant to halt, and no member could be told why. A person deals with a crash on one.
        decline(connection, instance_id, seq_no, pubsub_itc_fw_app::VoidDeclinedReason::NotAMembersCommand,
                command == CommandInRecord::NotDecodable ? "its record did not decode" : "it is not a member's new order or cancel");
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "HaltAndVoidRequests: matching engine instance {} died applying command seq={}, and no engine applied it and went on -- writing a VoidCommands "
               "command voiding it, so that every engine rejects it to its member instead of applying it (docs/availability/voided_commands.md, section 4)",
               instance_id, seq_no);
    static_cast<void>(links_.write_void_commands(pubsub_itc_fw_app::VoidCommands{seq_no, seq_no, false, pubsub_itc_fw_app::VoidReason::EngineDiedApplying}));
}

void HaltAndVoidRequests::decline(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int64_t seq_no,
                                  pubsub_itc_fw_app::VoidDeclinedReason reason, const char* why) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "HaltAndVoidRequests: matching engine instance {} died applying command seq={} and asked for it to be voided -- declined, because {} ({})",
               instance_id, seq_no, why, pubsub_itc_fw_app::to_string(reason));
    links_.decline_void(connection, seq_no, reason);
}

// ---- The halt when the venue cannot recover by itself ---------------------------------------------------------------

void HaltAndVoidRequests::recovery_halt(int64_t now_ns) {
    // 1. The orders held: no engine has been sent them, so each is rejected by the engine that next leads, rather than
    // applied once trading has halted. The cancels among them are carried out.
    void_held_orders_at_the_halt();

    // 2. Every order on the book, cancelled and reported to its member: they were locked out of their orders while no
    // engine could trade, and the market moved meanwhile. Not halting here: the TechnicalEvent below halts.
    pubsub_itc_fw_app::CancelBook cancel_book{};
    cancel_book.reason = pubsub_itc_fw_app::CancelBookReason::RecoveryTooLong;
    // Every order on the book: each was placed before this command.
    cancel_book.orders_placed_through = std::numeric_limits<int64_t>::max();
    static_cast<void>(links_.write_cancel_book(cancel_book));

    // 3. The halt itself.
    static_cast<void>(links_.write_technical_event(technical_event(Trading::halts, pubsub_itc_fw_app::TechnicalEventReason::RecoveryTooLong, now_ns)));
}

void HaltAndVoidRequests::void_held_orders_at_the_halt() {
    // Every command after the last one released has been sent to no engine: no engine is sent a record beyond the
    // released position (docs/availability/voided_commands.md, section 5.1). Nor is any command at or below one a report
    // in the WAL answers, because a report means an engine applied that command and went on.
    const int64_t first = std::max(links_.released_through(), links_.highest_command_answered()) + 1;
    const int64_t last = links_.last_seq_no();
    if (first > last) {
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Info, "HaltAndVoidRequests: no order is held at the halt, so nothing is voided");
        return;
    }
    // Only the new orders in the range: a held cancel applied late still does what its member asked, and every other
    // command in it is applied as usual.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "HaltAndVoidRequests: writing a VoidCommands for the new orders among commands seq={} to seq={}, held while no matching engine led -- the "
               "engine that next leads rejects each, saying trading halted while it was waiting",
               first, last);
    static_cast<void>(links_.write_void_commands(pubsub_itc_fw_app::VoidCommands{first, last, true, pubsub_itc_fw_app::VoidReason::HeldWhenTradingHalted}));
}

} // namespaces
