// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "TradingPhaseRequests.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

namespace {

// The position of a phase in the day: start of day first, end of day last.
int phase_order(pubsub_itc_fw_app::TradingPhase phase) {
    return static_cast<int>(phase);
}

// What members may do once the venue is in @p phase, for the answer to the operator.
std::string_view what_members_may_do(pubsub_itc_fw_app::TradingPhase phase) {
    switch (phase) {
        case pubsub_itc_fw_app::TradingPhase::PreOpen:
        case pubsub_itc_fw_app::TradingPhase::Open:
            return "members' orders and cancels are accepted";
        case pubsub_itc_fw_app::TradingPhase::StartOfDay:
        case pubsub_itc_fw_app::TradingPhase::Closed:
        case pubsub_itc_fw_app::TradingPhase::EndOfDay:
            break;
    }
    return "members' orders and cancels are refused";
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
    if (phase == pubsub_itc_fw_app::TradingPhase::StartOfDay) {
        start_of_day(connection, operator_name, trading_day);
        return;
    }
    move_on(connection, operator_name, phase);
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

void TradingPhaseRequests::move_on(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::TradingPhase phase) {
    const TradingPhaseState& state = links_.trading_phase();
    if (!state.started()) {
        answer(
            connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotNext, 0,
            fmt::format("the WAL holds no start of day yet, and every other phase follows it; phase {} was not written", pubsub_itc_fw_app::to_string(phase)));
        return;
    }
    const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
    if (phase_order(phase) != phase_order(current) + 1) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotNext, 0,
               fmt::format("the venue is in phase {}, and phase {} is not the one after it; phases are entered one at a time, in order, and nothing was "
                           "written",
                           pubsub_itc_fw_app::to_string(current), pubsub_itc_fw_app::to_string(phase)));
        return;
    }
    // The phase moves on while trading is halted, except while halted because a matching engine cannot account for the
    // orders the venue holds: nobody then knows which orders are open, so none can be cancelled at the close, and a person
    // resolves the halt first (docs/venue/trading_day_plan.md, section 2.6).
    const TradingHaltState& halt = links_.trading_halt();
    if (halt.halted() && halt.latest()->reason == pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0,
               fmt::format("trading is halted by the TechnicalEvent at seq={} because a matching engine cannot account for the orders the venue holds; a "
                           "person resolves that halt before the venue moves on, and nothing was written",
                           halt.latest()->seq_no));
        return;
    }
    // Pre-open waits until the leading engine has acted on the start of day: every record before pre-open is one this
    // sequencer wrote for the start of the day, and the start of day is the last of them that the engine is sent. The end
    // of day waits until the leading engine has acted on the close, so that every report the close produced is in the WAL
    // before the end of day (section 2.5).
    if (phase == pubsub_itc_fw_app::TradingPhase::PreOpen || phase == pubsub_itc_fw_app::TradingPhase::EndOfDay) {
        const pubsub_itc_fw_app::TradingPhase waits_on =
            phase == pubsub_itc_fw_app::TradingPhase::PreOpen ? pubsub_itc_fw_app::TradingPhase::StartOfDay : pubsub_itc_fw_app::TradingPhase::Closed;
        const int64_t waits_on_seq_no = state.entered_at(waits_on).value_or(0);
        const std::string outstanding = reports_outstanding();
        if (engine_applied_through_ >= waits_on_seq_no && !outstanding.empty()) {
            links_.ask_engine_position();
            answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0,
                   fmt::format("the matching engine has acted on the TechnicalEvent entering phase {} at seq={}, but {}; it has been asked again, and the "
                               "request is to be sent again; nothing was written",
                               pubsub_itc_fw_app::to_string(waits_on), waits_on_seq_no, outstanding));
            return;
        }
        if (engine_applied_through_ < waits_on_seq_no) {
            const bool engine = links_.an_engine_takes_orders();
            if (engine) {
                links_.ask_engine_position();
            }
            const std::string_view waits_on_name = pubsub_itc_fw_app::to_string(waits_on);
            answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0,
                   engine ? fmt::format("the matching engine has not yet said it has acted on the TechnicalEvent entering phase {} at seq={} (it has said "
                                        "seq={}); it has been asked, and the request is to be sent again; nothing was written",
                                        waits_on_name, waits_on_seq_no, engine_applied_through_)
                          : fmt::format("no matching engine takes orders, so none has acted on the TechnicalEvent entering phase {} at seq={}; the request "
                                        "is to be sent again once one does, and nothing was written",
                                        waits_on_name, waits_on_seq_no));
            return;
        }
    }
    const int32_t trading_day = state.trading_day();
    const int64_t seq_no = write_phase(phase, trading_day);
    if (seq_no == 0) {
        answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0,
               fmt::format("the TechnicalEvent entering phase {} was not written, so nothing was", pubsub_itc_fw_app::to_string(phase)));
        return;
    }
    answer(connection, operator_name, pubsub_itc_fw_app::TradingPhaseOutcome::Written, seq_no,
           fmt::format("wrote the TechnicalEvent at seq={} entering phase {} on trading day {}; {}", seq_no, pubsub_itc_fw_app::to_string(phase), trading_day,
                       what_members_may_do(phase)));
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

void TradingPhaseRequests::engine_position(int64_t highest_applied, const EngineReports& reports) {
    // The engine answers on one connection, in order, so the latest answer says the most of its reports.
    engine_applied_through_ = std::max(engine_applied_through_, highest_applied);
    engine_reports_ = reports;
}

std::string TradingPhaseRequests::reports_outstanding() const {
    if (engine_reports_.held > 0) {
        return fmt::format("it holds {} report(s) not yet sent to every sequencer", engine_reports_.held);
    }
    if (engine_reports_.sent_through.number == 0) {
        return {};
    }
    const std::optional<EngineReportPosition> in_wal = links_.latest_engine_report_in_wal();
    if (!in_wal.has_value() || !engine_reports_.sent_through.is_before_or_at(*in_wal)) {
        return fmt::format("the WAL does not yet hold every report it has sent: it sent through report {} under epoch {}, and the WAL holds through {}",
                           engine_reports_.sent_through.number, engine_reports_.sent_through.epoch,
                           in_wal.has_value() ? fmt::format("report {} under epoch {}", in_wal->number, in_wal->epoch) : std::string("none"));
    }
    return {};
}

void TradingPhaseRequests::stopped_leading() {
    engine_applied_through_ = 0;
    engine_reports_ = EngineReports{};
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
