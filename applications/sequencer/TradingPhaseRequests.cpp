// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "TradingPhaseRequests.hpp"

#include <algorithm>
#include <optional>

#include <fmt/format.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

namespace {

// The position of a phase in the day: start of day first, end of day last.
int phase_order(pubsub_itc_fw_app::TradingPhase phase) {
    return static_cast<int>(phase);
}

} // namespaces

TradingPhaseRequests::TradingPhaseRequests(TradingPhaseRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

void TradingPhaseRequests::request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day,
                                   pubsub_itc_fw_app::TradingPhase phase) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "TradingPhaseRequests: operator {} asks for the venue to enter phase {} on trading day {}, on "
               "connection {}",
               operator_name, pubsub_itc_fw_app::to_string(phase), trading_day, connection.get_value());
    if (!links_.leading()) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotLeader, 0,
               "this sequencer does not lead, so it wrote nothing; the sequencer that leads acts on the request");
        return;
    }
    const TradingPhaseState& state = links_.trading_phase();
    if (trading_day != links_.trading_day_started_for()) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0,
               fmt::format("this sequencer was started for trading day {}, not {}, so nothing was written", links_.trading_day_started_for(), trading_day));
        return;
    }
    if (state.started() && trading_day != state.trading_day()) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0,
               fmt::format("the WAL holds trading day {}, not {}: it is from another day and must be archived before this one starts; nothing was written",
                           state.trading_day(), trading_day));
        return;
    }
    if (state.started()) {
        const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
        if (current == phase) {
            answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyInPhase, 0,
                   fmt::format("the venue is in phase {} already, entered at seq={}, so nothing was written", pubsub_itc_fw_app::to_string(phase),
                               state.entered_at(phase).value_or(0)));
            return;
        }
        if (phase_order(phase) < phase_order(current)) {
            answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyPast, 0,
                   fmt::format("the venue has passed phase {} already and is in phase {}, so nothing was written", pubsub_itc_fw_app::to_string(phase),
                               pubsub_itc_fw_app::to_string(current)));
            return;
        }
    }
    switch (phase) {
        case pubsub_itc_fw_app::TradingPhase::StartOfDay:
            start_of_day(connection, operator_name, trading_day);
            return;
        case pubsub_itc_fw_app::TradingPhase::PreOpen:
            pre_open(connection, operator_name);
            return;
        case pubsub_itc_fw_app::TradingPhase::Open:
        case pubsub_itc_fw_app::TradingPhase::Closed:
        case pubsub_itc_fw_app::TradingPhase::EndOfDay:
            break;
    }
    answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::Unsupported, 0,
           fmt::format("this release does not move the venue to phase {} on request: it enters open straight after pre-open, and does not yet close; "
                       "nothing was written",
                       pubsub_itc_fw_app::to_string(phase)));
}

void TradingPhaseRequests::start_of_day(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day) {
    // The start of day is the first record of the trading day's WAL. A WAL that holds records but no start of day was
    // written before the venue kept trading days, or by hand, and belongs to no day this sequencer can name.
    if (links_.last_seq_no() != 0) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0,
               fmt::format("the WAL holds records through seq={} but no start of day, so it belongs to no trading day this sequencer can name; it must be "
                           "archived before trading day {} starts, and nothing was written",
                           links_.last_seq_no(), trading_day));
        return;
    }
    const int64_t seq_no = write_phase(pubsub_itc_fw_app::TradingPhase::StartOfDay, trading_day);
    if (seq_no == 0) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0,
               "the TechnicalEvent entering the start of day was not written, so nothing was");
        return;
    }
    answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::Written, seq_no,
           fmt::format("wrote the TechnicalEvent at seq={} entering the start of trading day {}; members' commands are refused until pre-open", seq_no,
                       trading_day));
}

void TradingPhaseRequests::pre_open(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name) {
    const TradingPhaseState& state = links_.trading_phase();
    if (!state.started() || state.latest()->phase != pubsub_itc_fw_app::TradingPhase::StartOfDay) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotNext, 0,
               "the WAL holds no start of day yet, and pre-open follows it; nothing was written");
        return;
    }
    const TradingHaltState& halt = links_.trading_halt();
    if (halt.halted() && halt.latest()->reason == pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0,
               fmt::format("trading is halted by the TechnicalEvent at seq={} because a matching engine cannot account for the orders the venue holds; a "
                           "person resolves that halt before the venue moves on, and nothing was written",
                           halt.latest()->seq_no));
        return;
    }
    // Every record before pre-open is one this sequencer wrote for the start of the day, and the start of day is the last
    // of them that the engine is sent. The engine has acted on all of them once it has acted on that one.
    const int64_t start_of_day_seq_no = state.entered_at(pubsub_itc_fw_app::TradingPhase::StartOfDay).value_or(0);
    if (engine_applied_through_ < start_of_day_seq_no) {
        const bool engine = links_.an_engine_takes_orders();
        if (engine) {
            links_.ask_engine_position();
        }
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0,
               engine ? fmt::format("the matching engine has not yet said it has acted on the start of day at seq={} (it has said seq={}); it has been "
                                    "asked, and the request is to be sent again; nothing was written",
                                    start_of_day_seq_no, engine_applied_through_)
                      : fmt::format("no matching engine takes orders, so none has acted on the start of day at seq={}; the request is to be sent again once "
                                    "one does, and nothing was written",
                                    start_of_day_seq_no));
        return;
    }
    const int32_t trading_day = state.trading_day();
    const int64_t pre_open_seq_no = write_phase(pubsub_itc_fw_app::TradingPhase::PreOpen, trading_day);
    if (pre_open_seq_no == 0) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0,
               "the TechnicalEvent entering pre-open was not written, so nothing was");
        return;
    }
    // Until the venue closes on its own schedule, open follows pre-open at once, so that members trade as they did before
    // the trading day had phases (docs/venue/trading_day_plan.md, section 5, step 4).
    const int64_t open_seq_no = write_phase(pubsub_itc_fw_app::TradingPhase::Open, trading_day);
    if (open_seq_no == 0) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, pre_open_seq_no,
               fmt::format("wrote the TechnicalEvent at seq={} entering pre-open, but the one entering open was not written; asking for pre-open again "
                           "answers that the venue is in it",
                           pre_open_seq_no));
        return;
    }
    answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::Written, open_seq_no,
           fmt::format("wrote the TechnicalEvents at seq={} entering pre-open and at seq={} entering open, on trading day {}; members' commands are accepted",
                       pre_open_seq_no, open_seq_no, trading_day));
}

int64_t TradingPhaseRequests::write_phase(pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day) {
    pubsub_itc_fw_app::TechnicalEvent event{};
    event.phase = phase;
    event.halted = links_.trading_halt().halted();
    event.trading_day = trading_day;
    event.reason = pubsub_itc_fw_app::TechnicalEventReason::RequestedByOperator;
    const int64_t seq_no = links_.next_seq_no();
    return links_.write_technical_event(event) ? seq_no : 0;
}

void TradingPhaseRequests::engine_position(int64_t highest_applied) {
    engine_applied_through_ = std::max(engine_applied_through_, highest_applied);
}

void TradingPhaseRequests::stopped_leading() {
    engine_applied_through_ = 0;
}

void TradingPhaseRequests::answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::TradingPhaseOutcome outcome,
                                  int64_t technical_event_seq_no, const std::string& explanation) {
    const TradingPhaseState& state = links_.trading_phase();
    pubsub_itc_fw_app::TradingPhaseAnswer reply{};
    reply.outcome = outcome;
    reply.day_started = state.started();
    reply.trading_day = state.trading_day();
    reply.phase = state.started() ? state.latest()->phase : pubsub_itc_fw_app::TradingPhase::StartOfDay;
    reply.halted = links_.trading_halt().halted();
    reply.technical_event_seq_no = technical_event_seq_no;
    reply.explanation = explanation;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "TradingPhaseRequests: answering operator {} on connection {}: {} -- {}", operator_name,
               connection.get_value(), pubsub_itc_fw_app::to_string(outcome), explanation);
    links_.answer(connection, reply);
}

} // namespaces
