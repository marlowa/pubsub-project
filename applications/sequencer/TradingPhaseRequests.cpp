// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "TradingPhaseRequests.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        case pubsub_itc_fw_app::TradingPhase::EndOfDayBegin:
        case pubsub_itc_fw_app::TradingPhase::EndOfDayEnd:
            break;
    }
    return "members' orders and cancels are refused";
}

} // namespaces

TradingPhaseRequests::TradingPhaseRequests(TradingPhaseRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger, int32_t end_of_day_publishers,
                                           int64_t longest_wait_for_copies_ns)
    : links_(links), logger_(logger), copies_(end_of_day_publishers, longest_wait_for_copies_ns), longest_wait_ns_(longest_wait_for_copies_ns) {}

void TradingPhaseRequests::request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day,
                                   pubsub_itc_fw_app::TradingPhase phase, const std::vector<ScheduledPhase>& schedule) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "TradingPhaseRequests: operator {} asks for the venue to enter phase {} on trading day {}, on "
               "connection {}{}",
               operator_name, pubsub_itc_fw_app::to_string(phase), trading_day, connection.get_value(),
               phase == pubsub_itc_fw_app::TradingPhase::StartOfDay ? fmt::format(", with the schedule: {}", schedule_in_words(schedule)) : std::string());
    if (!links_.leading()) {
        answer(connection, operator_name,
               Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotLeader, 0, 0,
                       "this sequencer does not lead, so it wrote nothing; the sequencer that leads acts on the request"});
        return;
    }
    const TradingPhaseState& state = links_.trading_phase();
    if (trading_day != links_.trading_day_started_for()) {
        answer(connection, operator_name,
               Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0, 0,
                       fmt::format("this sequencer was started for trading day {}, not {}, so nothing was written", links_.trading_day_started_for(),
                                   trading_day)});
        return;
    }
    if (state.started() && trading_day != state.trading_day()) {
        answer(connection, operator_name,
               Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0, 0,
                       fmt::format("the WAL holds trading day {}, not {}: it is from another day and must be archived before this one starts; nothing was "
                                   "written",
                                   state.trading_day(), trading_day)});
        return;
    }
    if (phase == pubsub_itc_fw_app::TradingPhase::StartOfDay) {
        answer(connection, operator_name, start_of_day(operator_name, trading_day, schedule));
        return;
    }
    if (state.started()) {
        const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
        if (current == phase) {
            answer(connection, operator_name,
                   Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyInPhase, 0, 0,
                           fmt::format("the venue is in phase {} already, entered at seq={}, so nothing was written", pubsub_itc_fw_app::to_string(phase),
                                       state.entered_at(phase).value_or(0))});
            return;
        }
        if (phase_order(phase) < phase_order(current)) {
            answer(connection, operator_name,
                   Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyPast, 0, 0,
                           fmt::format("the venue has passed phase {} already and is in phase {}, so nothing was written", pubsub_itc_fw_app::to_string(phase),
                                       pubsub_itc_fw_app::to_string(current))});
            return;
        }
    }
    if (phase == pubsub_itc_fw_app::TradingPhase::EndOfDayEnd) {
        // Asking for it would let the components shut down before every copy of the WAL is complete, which is what the two
        // events of the end of day prevent (docs/venue/start_and_end_of_day.md, section 4.2).
        const std::string progress = copies_last_logged_.empty() ? std::string() : fmt::format("; at the last check, {}", copies_last_logged_);
        answer(connection, operator_name,
               Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::WrittenByTheVenue, 0, 0,
                       fmt::format("the end-of-day end is never asked for: the leading sequencer writes it by itself, once the venue is in the "
                                   "end-of-day begin and every copy of the WAL holds the begin, or once the limit of time has passed; nothing was "
                                   "written{}",
                                   progress)});
        return;
    }
    answer(connection, operator_name, move_on(phase, pubsub_itc_fw_app::TechnicalEventReason::RequestedByOperator));
}

void TradingPhaseRequests::state_query(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "TradingPhaseRequests: operator {} asks for the trading state, on connection {}", operator_name,
               connection.get_value());
    if (!links_.leading()) {
        answer(connection, operator_name,
               Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotLeader, 0, 0,
                       "this sequencer does not lead; the state given is the one its WAL holds, and the sequencer that leads gives the venue's"},
               pubsub_itc_fw::FwLogLevel::Info);
        return;
    }
    const TradingPhaseState& state = links_.trading_phase();
    answer(connection, operator_name,
           Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::StateOnly, 0, 0,
                   state.started()
                       ? fmt::format("the venue is in phase {} of trading day {}, entered at seq={}", pubsub_itc_fw_app::to_string(state.latest()->phase),
                                     state.trading_day(), state.entered_at(state.latest()->phase).value_or(0))
                       : std::string("the WAL holds no start of day")},
           pubsub_itc_fw::FwLogLevel::Info);
}

TradingPhaseRequests::Attempt TradingPhaseRequests::start_of_day(std::string_view operator_name, int32_t trading_day,
                                                                 const std::vector<ScheduledPhase>& schedule) {
    const TradingPhaseState& state = links_.trading_phase();
    if (state.started()) {
        const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
        const int64_t entered = state.entered_at(pubsub_itc_fw_app::TradingPhase::StartOfDay).value_or(0);
        if (current != pubsub_itc_fw_app::TradingPhase::StartOfDay) {
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyPast, 0, 0,
                           fmt::format("the venue has passed the start of day already and is in phase {}, so nothing was written; the times that remain are "
                                       "replaced by a TradingScheduleRequest",
                                       pubsub_itc_fw_app::to_string(current))};
        }
        if (links_.trading_schedule().recorded()) {
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::AlreadyInPhase, 0, 0,
                           fmt::format("the venue is in the start of day already, entered at seq={}, with the schedule at seq={}, so nothing was written; "
                                       "the times that remain are replaced by a TradingScheduleRequest",
                                       entered, links_.trading_schedule().latest()->seq_no)};
        }
        // The leader that wrote the start of day stopped before it wrote the schedule: the request sent again writes the
        // schedule alone.
        const std::string problem = schedule_problem(pubsub_itc_fw_app::TradingPhase::StartOfDay, schedule);
        if (!problem.empty()) {
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::ScheduleRefused, 0, 0,
                           fmt::format("the venue is in the start of day, entered at seq={}, with no schedule, and the schedule sent is not one it can "
                                       "follow: {}; nothing was written",
                                       entered, problem)};
        }
        const int64_t schedule_seq_no = write_schedule(operator_name, trading_day, schedule);
        if (schedule_seq_no == 0) {
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0, 0,
                           "the venue is in the start of day with no schedule, and the TradingSchedule was not written, so nothing was"};
        }
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::Written, 0, schedule_seq_no,
                       fmt::format("the venue is in the start of day already, entered at seq={}, with no schedule; wrote the TradingSchedule at seq={}: {}",
                                   entered, schedule_seq_no, schedule_in_words(schedule))};
    }
    // The start of day is the first record of the trading day's WAL. A WAL that holds records but no start of day was
    // written before the venue kept trading days, or by hand, and belongs to no day this sequencer can name.
    if (links_.last_seq_no() != 0) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::WrongTradingDay, 0, 0,
                       fmt::format("the WAL holds records through seq={} but no start of day, so it belongs to no trading day this sequencer can name; it "
                                   "must be archived before trading day {} starts, and nothing was written",
                                   links_.last_seq_no(), trading_day)};
    }
    const std::string problem = schedule_problem(pubsub_itc_fw_app::TradingPhase::StartOfDay, schedule);
    if (!problem.empty()) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::ScheduleRefused, 0, 0,
                       fmt::format("the schedule sent with the start of day is not one the venue can follow: {}; nothing was written", problem)};
    }
    const int64_t seq_no = write_phase(pubsub_itc_fw_app::TradingPhase::StartOfDay, trading_day, pubsub_itc_fw_app::TechnicalEventReason::RequestedByOperator);
    if (seq_no == 0) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0, 0,
                       "the TechnicalEvent entering the start of day was not written, so nothing was"};
    }
    const int64_t schedule_seq_no = write_schedule(operator_name, trading_day, schedule);
    if (schedule_seq_no == 0) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, seq_no, 0,
                       fmt::format("wrote the TechnicalEvent at seq={} entering the start of trading day {}, but the TradingSchedule was not written; the "
                                   "request sent again writes it",
                                   seq_no, trading_day)};
    }
    return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::Written, seq_no, schedule_seq_no,
                   fmt::format("wrote the TechnicalEvent at seq={} entering the start of trading day {}, and the TradingSchedule at seq={}: {}; members' "
                               "commands are refused until pre-open",
                               seq_no, trading_day, schedule_seq_no, schedule_in_words(schedule))};
}

int64_t TradingPhaseRequests::write_schedule(std::string_view operator_name, int32_t trading_day, const std::vector<ScheduledPhase>& schedule) {
    std::vector<pubsub_itc_fw_app::TradingScheduleEntry> entries;
    entries.reserve(schedule.size());
    for (const ScheduledPhase& entry : schedule) {
        entries.push_back(pubsub_itc_fw_app::TradingScheduleEntry{entry.phase, entry.earliest_utc_ns});
    }
    pubsub_itc_fw_app::TradingSchedule record{};
    record.trading_day = trading_day;
    record.entries = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::TradingScheduleEntry>{entries.data(), entries.size()};
    record.operator_name = operator_name;
    const int64_t seq_no = links_.next_seq_no();
    return links_.write_trading_schedule(record) ? seq_no : 0;
}

void TradingPhaseRequests::schedule_request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day,
                                            const std::vector<ScheduledPhase>& schedule) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "TradingPhaseRequests: operator {} asks for the schedule of trading day {} to be replaced, on connection {}, by: {}", operator_name, trading_day,
               connection.get_value(), schedule_in_words(schedule));
    if (!links_.leading()) {
        answer_schedule(connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::NotLeader, 0,
                        "this sequencer does not lead, so it wrote nothing; the sequencer that leads acts on the request");
        return;
    }
    if (trading_day != links_.trading_day_started_for()) {
        answer_schedule(
            connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::WrongTradingDay, 0,
            fmt::format("this sequencer was started for trading day {}, not {}, so nothing was written", links_.trading_day_started_for(), trading_day));
        return;
    }
    const TradingPhaseState& state = links_.trading_phase();
    if (!state.started()) {
        answer_schedule(connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::TradingDayNotStarted, 0,
                        "the WAL holds no start of day, so there is no schedule to replace; the day's schedule is sent with the start-of-day request, and "
                        "nothing was written");
        return;
    }
    if (trading_day != state.trading_day()) {
        answer_schedule(connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::WrongTradingDay, 0,
                        fmt::format("the WAL holds trading day {}, not {}, so nothing was written", state.trading_day(), trading_day));
        return;
    }
    const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
    const std::string problem = schedule_problem(current, schedule);
    if (!problem.empty()) {
        answer_schedule(
            connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::Refused, 0,
            fmt::format("the schedule is not one the venue can follow from phase {}: {}; nothing was written", pubsub_itc_fw_app::to_string(current), problem));
        return;
    }
    const int64_t seq_no = write_schedule(operator_name, trading_day, schedule);
    if (seq_no == 0) {
        answer_schedule(connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::NotWritten, 0,
                        "the TradingSchedule was not written, so the schedule is as it was");
        return;
    }
    answer_schedule(connection, operator_name, pubsub_itc_fw_app::TradingScheduleOutcome::Written, seq_no,
                    fmt::format("wrote the TradingSchedule at seq={}, which replaces the schedule before it: {}", seq_no, schedule_in_words(schedule)));
}

void TradingPhaseRequests::scheduled_check(int64_t now_utc_ns, int64_t check_interval_ns) {
    const TradingPhaseState& state = links_.trading_phase();
    if (!links_.leading() || !state.started()) {
        return;
    }
    const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
    if (current == pubsub_itc_fw_app::TradingPhase::EndOfDayEnd) {
        // Only for an end this instance wrote: one that started on a WAL already ending the day, as a check does on a copy
        // of a day's WAL, goes on serving it. A leader restarted after the end has already exited on its last-state file.
        if (end_written_here_) {
            stop_once_the_follower_holds_the_end(now_utc_ns);
        }
        return;
    }
    if (current == pubsub_itc_fw_app::TradingPhase::EndOfDayBegin) {
        end_of_day_end_check(now_utc_ns);
        return;
    }
    const auto next = static_cast<pubsub_itc_fw_app::TradingPhase>(phase_order(current) + 1);
    const std::optional<ScheduledPhase> entry = links_.trading_schedule().entry_for(next);
    if (!entry.has_value()) {
        return;
    }
    if (entry->earliest_utc_ns > now_utc_ns) {
        // The engine is asked one check ahead, so that its answer is here when the time comes, and a phase that waits on it
        // is entered at the check its time comes, not the one after.
        if (entry->earliest_utc_ns - now_utc_ns <= check_interval_ns) {
            ask_engine_ahead_of(next);
        }
        return;
    }
    const Attempt attempt = move_on(next, pubsub_itc_fw_app::TechnicalEventReason::Scheduled);
    const std::string when =
        entry->earliest_utc_ns == 0 ? std::string("as soon as its conditions hold") : fmt::format("at {}", utc_time_text(entry->earliest_utc_ns));
    if (attempt.outcome == pubsub_itc_fw_app::TradingPhaseOutcome::Written) {
        scheduled_check_waiting_.clear();
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "TradingPhaseRequests: the schedule enters phase {} {}, and the time has come ({} by this sequencer's clock): {}",
                   pubsub_itc_fw_app::to_string(next), when, utc_time_text(now_utc_ns), attempt.explanation);
        return;
    }
    const pubsub_itc_fw::FwLogLevel level =
        attempt.explanation == scheduled_check_waiting_ ? pubsub_itc_fw::FwLogLevel::Debug : pubsub_itc_fw::FwLogLevel::Warning;
    PUBSUB_LOG(logger_, level, "TradingPhaseRequests: the schedule enters phase {} {}, and the time has come, but it was not entered: {} -- {}",
               pubsub_itc_fw_app::to_string(next), when, pubsub_itc_fw_app::to_string(attempt.outcome), attempt.explanation);
    scheduled_check_waiting_ = attempt.explanation;
}

void TradingPhaseRequests::ask_engine_ahead_of(pubsub_itc_fw_app::TradingPhase phase) {
    if (phase != pubsub_itc_fw_app::TradingPhase::PreOpen && phase != pubsub_itc_fw_app::TradingPhase::EndOfDayBegin) {
        return;
    }
    const pubsub_itc_fw_app::TradingPhase waits_on =
        phase == pubsub_itc_fw_app::TradingPhase::PreOpen ? pubsub_itc_fw_app::TradingPhase::StartOfDay : pubsub_itc_fw_app::TradingPhase::Closed;
    const std::optional<int64_t> waits_on_seq_no = links_.trading_phase().entered_at(waits_on);
    if (waits_on_seq_no.has_value() && engine_applied_through_ < *waits_on_seq_no && links_.an_engine_takes_orders()) {
        links_.ask_engine_position();
    }
}

TradingPhaseRequests::Attempt TradingPhaseRequests::move_on(pubsub_itc_fw_app::TradingPhase phase, pubsub_itc_fw_app::TechnicalEventReason reason) {
    const TradingPhaseState& state = links_.trading_phase();
    if (!state.started()) {
        return Attempt{
            pubsub_itc_fw_app::TradingPhaseOutcome::NotNext, 0, 0,
            fmt::format("the WAL holds no start of day yet, and every other phase follows it; phase {} was not written", pubsub_itc_fw_app::to_string(phase))};
    }
    const pubsub_itc_fw_app::TradingPhase current = state.latest()->phase;
    if (phase_order(phase) != phase_order(current) + 1) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotNext, 0, 0,
                       fmt::format("the venue is in phase {}, and phase {} is not the one after it; phases are entered one at a time, in order, and "
                                   "nothing was written",
                                   pubsub_itc_fw_app::to_string(current), pubsub_itc_fw_app::to_string(phase))};
    }
    // The phase moves on while trading is halted, except while halted because a matching engine cannot account for the
    // orders the venue holds: nobody then knows which orders are open, so none can be cancelled at the close, and a person
    // resolves the halt first (docs/venue/trading_day_plan.md, section 2.6).
    const TradingHaltState& halt = links_.trading_halt();
    if (halt.halted() && halt.latest()->reason == pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0, 0,
                       fmt::format("trading is halted by the TechnicalEvent at seq={} because a matching engine cannot account for the orders the venue "
                                   "holds; a person resolves that halt before the venue moves on, and nothing was written",
                                   halt.latest()->seq_no)};
    }
    // Pre-open waits until the leading engine has acted on the start of day: every record before pre-open is one this
    // sequencer wrote for the start of the day, and the start of day is the last of them that the engine is sent. The end
    // of day waits until the leading engine has acted on the close, so that every report the close produced is in the WAL
    // before the end of day (section 2.5).
    if (phase == pubsub_itc_fw_app::TradingPhase::PreOpen || phase == pubsub_itc_fw_app::TradingPhase::EndOfDayBegin) {
        const pubsub_itc_fw_app::TradingPhase waits_on =
            phase == pubsub_itc_fw_app::TradingPhase::PreOpen ? pubsub_itc_fw_app::TradingPhase::StartOfDay : pubsub_itc_fw_app::TradingPhase::Closed;
        const int64_t waits_on_seq_no = state.entered_at(waits_on).value_or(0);
        const std::string outstanding = reports_outstanding();
        if (engine_applied_through_ >= waits_on_seq_no && !outstanding.empty()) {
            links_.ask_engine_position();
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0, 0,
                           fmt::format("the matching engine has acted on the TechnicalEvent entering phase {} at seq={}, but {}; it has been asked again, "
                                       "and nothing was written",
                                       pubsub_itc_fw_app::to_string(waits_on), waits_on_seq_no, outstanding)};
        }
        if (engine_applied_through_ < waits_on_seq_no) {
            const bool engine = links_.an_engine_takes_orders();
            if (engine) {
                links_.ask_engine_position();
            }
            const std::string_view waits_on_name = pubsub_itc_fw_app::to_string(waits_on);
            return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotReady, 0, 0,
                           engine ? fmt::format("the matching engine has not yet said it has acted on the TechnicalEvent entering phase {} at seq={} (it "
                                                "has said seq={}); it has been asked, and nothing was written",
                                                waits_on_name, waits_on_seq_no, engine_applied_through_)
                                  : fmt::format("no matching engine takes orders, so none has acted on the TechnicalEvent entering phase {} at seq={}; "
                                                "nothing was written",
                                                waits_on_name, waits_on_seq_no)};
        }
    }
    const int32_t trading_day = state.trading_day();
    const int64_t seq_no = write_phase(phase, trading_day, reason);
    if (seq_no == 0) {
        return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten, 0, 0,
                       fmt::format("the TechnicalEvent entering phase {} was not written, so nothing was", pubsub_itc_fw_app::to_string(phase))};
    }
    return Attempt{pubsub_itc_fw_app::TradingPhaseOutcome::Written, seq_no, 0,
                   fmt::format("wrote the TechnicalEvent at seq={} entering phase {} on trading day {}; {}", seq_no, pubsub_itc_fw_app::to_string(phase),
                               trading_day, what_members_may_do(phase))};
}

int64_t TradingPhaseRequests::write_phase(pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day, pubsub_itc_fw_app::TechnicalEventReason reason) {
    pubsub_itc_fw_app::TechnicalEvent event{};
    event.phase = phase;
    event.halted = links_.trading_halt().halted();
    event.trading_day = trading_day;
    event.reason = reason;
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
    copies_.forget();
    copies_last_logged_.clear();
}

void TradingPhaseRequests::publisher_holds(const pubsub_itc_fw::ConnectionID& connection, int64_t last_seq_no) {
    copies_.publisher_holds(connection, last_seq_no);
}

void TradingPhaseRequests::publisher_gone(const pubsub_itc_fw::ConnectionID& connection) {
    copies_.publisher_gone(connection);
}

void TradingPhaseRequests::end_of_day_end_check(int64_t now_utc_ns) {
    const TradingPhaseState& state = links_.trading_phase();
    const int64_t begin_seq_no = state.entered_at(pubsub_itc_fw_app::TradingPhase::EndOfDayBegin).value_or(state.latest()->seq_no);
    copies_.start(begin_seq_no, now_utc_ns);
    const EndOfDayCopies::Assessment assessment = copies_.assess(links_.follower_sequencer_holds_through(), now_utc_ns);
    if (assessment.verdict == EndOfDayCopies::Verdict::Waiting) {
        links_.ask_publishers_what_they_hold(begin_seq_no);
        if (assessment.explanation != copies_last_logged_) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "TradingPhaseRequests: the end-of-day end is not yet written -- {}", assessment.explanation);
            copies_last_logged_ = assessment.explanation;
        }
        return;
    }
    const bool complete = assessment.verdict == EndOfDayCopies::Verdict::EveryCopyComplete;
    const pubsub_itc_fw_app::TechnicalEventReason reason =
        complete ? pubsub_itc_fw_app::TechnicalEventReason::EveryCopyComplete : pubsub_itc_fw_app::TechnicalEventReason::CopiesNotCompleteInTime;
    const int64_t seq_no = write_phase(pubsub_itc_fw_app::TradingPhase::EndOfDayEnd, state.trading_day(), reason);
    end_written_here_ = seq_no != 0;
    if (seq_no == 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "TradingPhaseRequests: the TechnicalEvent entering the end-of-day end was not written; the next check tries again -- {}",
                   assessment.explanation);
        return;
    }
    copies_last_logged_.clear();
    if (complete) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "TradingPhaseRequests: wrote the TechnicalEvent at seq={} entering the end-of-day end of trading day {}, because every copy of the WAL "
                   "holds the end-of-day begin -- {}",
                   seq_no, state.trading_day(), assessment.explanation);
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "TradingPhaseRequests: wrote the TechnicalEvent at seq={} entering the end-of-day end of trading day {} with a copy of the WAL still "
               "incomplete, because the limit of time for the copies has passed; the copy that is incomplete may lack the end of the day's record -- {}",
               seq_no, state.trading_day(), assessment.explanation);
}

void TradingPhaseRequests::stop_once_the_follower_holds_the_end(int64_t now_utc_ns) {
    if (stop_asked_) {
        return;
    }
    const TradingPhaseState& state = links_.trading_phase();
    const int64_t end_seq_no = state.entered_at(pubsub_itc_fw_app::TradingPhase::EndOfDayEnd).value_or(state.latest()->seq_no);
    if (!end_seen_at_ns_.has_value()) {
        end_seen_at_ns_ = now_utc_ns;
    }
    const std::optional<int64_t> follower = links_.follower_sequencer_holds_through();
    if (follower.has_value() && *follower < end_seq_no) {
        if (now_utc_ns - *end_seen_at_ns_ < longest_wait_ns_) {
            return;
        }
        // The follower may hold the end and its acknowledgement have been lost with the connection, or it may not: this
        // instance cannot know, and goes on without it (decision 4, case 5).
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "TradingPhaseRequests: the follower sequencer has not acknowledged the end-of-day end at seq={} within {} s -- it holds records through "
                   "{}; stopping for the end of day without its acknowledgement, so its WAL may lack the end",
                   end_seq_no, longest_wait_ns_ / 1'000'000'000, *follower);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "TradingPhaseRequests: {} -- stopping for the end of day",
                   follower.has_value()
                       ? fmt::format("the follower sequencer has acknowledged the end-of-day end at seq={}, so both sequencers' WALs end with it", end_seq_no)
                       : fmt::format("the end-of-day end is at seq={}, and there is no follower sequencer to acknowledge it, because high availability is off",
                                     end_seq_no));
    }
    stop_asked_ = true;
    links_.stop_for_end_of_day(end_seq_no);
}

void TradingPhaseRequests::answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, const Attempt& attempt,
                                  pubsub_itc_fw::FwLogLevel level) {
    const TradingPhaseState& state = links_.trading_phase();
    pubsub_itc_fw_app::TradingPhaseAnswer reply{};
    reply.outcome = attempt.outcome;
    reply.day_started = state.started();
    reply.trading_day = state.trading_day();
    reply.phase = state.started() ? state.latest()->phase : pubsub_itc_fw_app::TradingPhase::StartOfDay;
    reply.halted = links_.trading_halt().halted();
    reply.technical_event_seq_no = attempt.technical_event_seq_no;
    reply.schedule_seq_no = attempt.schedule_seq_no;
    reply.explanation = attempt.explanation;
    PUBSUB_LOG(logger_, level, "TradingPhaseRequests: answering operator {} on connection {}: {} -- {}", operator_name, connection.get_value(),
               pubsub_itc_fw_app::to_string(attempt.outcome), attempt.explanation);
    links_.answer(connection, reply);
}

void TradingPhaseRequests::answer_schedule(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name,
                                           pubsub_itc_fw_app::TradingScheduleOutcome outcome, int64_t schedule_seq_no, const std::string& explanation) {
    const TradingPhaseState& state = links_.trading_phase();
    pubsub_itc_fw_app::TradingScheduleAnswer reply{};
    reply.outcome = outcome;
    reply.day_started = state.started();
    reply.trading_day = state.trading_day();
    reply.phase = state.started() ? state.latest()->phase : pubsub_itc_fw_app::TradingPhase::StartOfDay;
    reply.halted = links_.trading_halt().halted();
    reply.schedule_seq_no = schedule_seq_no;
    reply.explanation = explanation;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "TradingPhaseRequests: answering operator {} on connection {}: {} -- {}", operator_name,
               connection.get_value(), pubsub_itc_fw_app::to_string(outcome), explanation);
    links_.answer(connection, reply);
}

} // namespaces
