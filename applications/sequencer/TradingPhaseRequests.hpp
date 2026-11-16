#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>

#include "EndOfDayCopies.hpp"
#include "EngineReportPosition.hpp"
#include "TradingHaltState.hpp"
#include "TradingPhaseState.hpp"
#include "TradingScheduleState.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that TradingPhaseRequests can read the facts it decides from, write a
 *        TechnicalEvent or a TradingSchedule into the WAL, ask the matching engine how far it has got, and answer the one
 *        who asked.
 */
class TradingPhaseRequestsLinksInterface {
  public:
    virtual ~TradingPhaseRequestsLinksInterface() = default;

    /// Whether this instance leads, and so writes commands.
    [[nodiscard]] virtual bool leading() const = 0;
    /// The trading day this sequencer was started for, as the number yyyymmdd (its --trading-day argument).
    [[nodiscard]] virtual int32_t trading_day_started_for() const = 0;
    /// The trading day and the phase, as the WAL says.
    [[nodiscard]] virtual const TradingPhaseState& trading_phase() const = 0;
    /// The day's schedule, as the WAL says.
    [[nodiscard]] virtual const TradingScheduleState& trading_schedule() const = 0;
    /// Whether trading is halted, and by which TechnicalEvent, as the WAL says.
    [[nodiscard]] virtual const TradingHaltState& trading_halt() const = 0;
    /// The last record in the WAL; zero when the WAL holds none.
    [[nodiscard]] virtual int64_t last_seq_no() const = 0;
    /// The number the next record written will have.
    [[nodiscard]] virtual int64_t next_seq_no() const = 0;
    /// The latest of the matching engine's reports in the WAL, by the engine's epoch and its number for the report.
    [[nodiscard]] virtual std::optional<EngineReportPosition> latest_engine_report_in_wal() const = 0;
    /// Whether a matching engine takes orders now.
    [[nodiscard]] virtual bool an_engine_takes_orders() const = 0;
    /// Asks the matching engine orders are routed to for the highest command it has acted on (EnginePositionQuery); its
    /// answer comes to TradingPhaseRequests::engine_position.
    virtual void ask_engine_position() = 0;
    /// The last record the follower sequencer has acknowledged holding, or nothing when high availability is off and there
    /// is no follower sequencer.
    [[nodiscard]] virtual std::optional<int64_t> follower_sequencer_holds_through() const = 0;
    /// Asks every matching engine publisher connected for the last record its copy of the WAL holds (WalHeldRequest),
    /// saying it waits for @p wanted_through; each answer comes to TradingPhaseRequests::publisher_holds.
    virtual void ask_publishers_what_they_hold(int64_t wanted_through) = 0;
    /// Writes @p event into the WAL as a command of the sequencer's own. @return false if it was not written.
    virtual bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) = 0;
    /// Writes @p schedule into the WAL as a record of the sequencer's own, which no engine applies. @return false if it was
    /// not written.
    virtual bool write_trading_schedule(const pubsub_itc_fw_app::TradingSchedule& schedule) = 0;
    /// Sends the one on @p connection @p answer.
    virtual void answer(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw_app::TradingPhaseAnswer& answer) = 0;
    /// Sends the one on @p connection @p answer.
    virtual void answer(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw_app::TradingScheduleAnswer& answer) = 0;
};

/**
 * @brief Moves the venue from one phase of the trading day to the next, when an operator asks or when the time in the
 *        day's schedule comes, and records and replaces the schedule (docs/venue/trading_day_plan.md and
 *        docs/venue/trading_day_and_reference_data.md, "Moving from one phase to the next").
 *
 * Only the leader acts. Every request is answered from the sequencer's current state, so a request sent again, after an
 * answer was lost or a change of leader, does no harm. The phases are entered one at a time, in order: start of day,
 * pre-open, open, closed, the end-of-day begin and the end-of-day end. What is done with each request:
 *
 * - To a sequencer that does not lead: NotLeader, and nothing is written.
 * - For a trading day other than the one this sequencer was started for, or the one the WAL holds: WrongTradingDay, and
 *   nothing is written.
 * - For the phase the venue is in: AlreadyInPhase. For a phase the venue has passed: AlreadyPast. Nothing is written.
 * - For the start of day, on a WAL that holds no record: the schedule sent with the request is checked
 *   (schedule_problem), and if it is not one the venue can follow, ScheduleRefused, and nothing is written. Otherwise the
 *   TechnicalEvent entering the start of day is written as the WAL's first record, and the TradingSchedule straight after
 *   it, empty for a day run by hand. On a WAL that holds the start of day without a schedule, because the leader stopped
 *   between the two, the schedule alone is written. On a WAL that holds records but no start of day: WrongTradingDay,
 *   because the WAL belongs to no trading day and must be archived.
 * - For any later phase that is not the one after the current phase: NotNext, and nothing is written.
 * - For the next phase, while trading is halted because a matching engine cannot account for the orders the venue holds:
 *   NotReady. A person resolves that halt before the venue moves on. Any other halt does not stop the phase moving on.
 * - For the end-of-day end, which the venue is not in: WrittenByTheVenue, and nothing is written. Only the leading
 *   sequencer writes it, by itself (below).
 * - For pre-open, until the leading matching engine has said it has acted on the start of day; and for the end-of-day
 *   begin, until it has said it has acted on the close: NotReady, and the engine is asked how far it has got. The request is to be
 *   sent again. The engine's answer must also show that every report it has sent is in the WAL, and that it holds none
 *   unsent: the close produces a report for each order it cancels, and the end of day is not written until all of them are
 *   in the WAL, so that no order record follows it (docs/venue/trading_day_plan.md, section 2.5). The reports travel on
 *   other connections from the answer, so the answer can arrive first; the sequencer compares the engine's count of its
 *   reports with the latest report in the WAL.
 * - Otherwise, the TechnicalEvent entering the phase is written.
 *
 * Every change of phase carries whether trading is halted, so a halt is carried into the next phase. Every request and
 * its answer are logged at Warning with the operator's name.
 *
 * Replacing the schedule (schedule_request): the new schedule must start at the phase after the one the venue is in, so it
 * never names a phase the venue has entered or passed. It is written as a new TradingSchedule, which replaces the one
 * before it from its position in the WAL onwards.
 *
 * Following the schedule (scheduled_check, once a second): when the latest schedule gives a time for the phase after the
 * current one, and that time has come by the leader's clock, the phase is entered exactly as an operator's request would
 * enter it, with the same conditions, and with the reason Scheduled. If a condition does not hold, nothing is written,
 * and the next check tries again, so the phase is entered as soon as it does. A change is written only while the venue is
 * in the phase before it, so a check repeated, or an operator's request in the same second, or a new leader, leaves one
 * record (section 3.3 of the plan).
 *
 * Ending the day (scheduled_check, in the end-of-day begin): the end-of-day end is written once the follower sequencer
 * and every matching engine publisher waited for hold every record up to the begin, with the reason EveryCopyComplete,
 * or once the limit of time has passed, with the reason CopiesNotCompleteInTime and a Warning naming the copy that is
 * incomplete (EndOfDayCopies; docs/venue/start_and_end_of_day.md, section 4.2). Until then the publishers are asked
 * once a second how far their copies go. Once the end is written, the moment the follower sequencer acknowledges it is
 * logged.
 *
 * One thread, the sequencer's, calls every member function.
 */
class TradingPhaseRequests {
  public:
    /**
     * @param[in] links                       The sequencer thread.
     * @param[in] logger                      The Quill log.
     * @param[in] end_of_day_publishers       How many matching engine publishers must hold the end-of-day begin before the
     *                                        end-of-day end is written.
     * @param[in] longest_wait_for_copies_ns  How long the copies are waited for at most, in nanoseconds.
     */
    TradingPhaseRequests(TradingPhaseRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger, int32_t end_of_day_publishers,
                         int64_t longest_wait_for_copies_ns);

    /// Operator @p operator_name, on @p connection, asks for the venue to enter @p phase on trading day @p trading_day.
    /// With the start of day, @p schedule is the day's schedule; with any other phase it is not read.
    void request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day, pubsub_itc_fw_app::TradingPhase phase,
                 const std::vector<ScheduledPhase>& schedule = {});

    /// Operator @p operator_name, on @p connection, asks for the times that remain on trading day @p trading_day to be
    /// replaced by @p schedule.
    void schedule_request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day,
                          const std::vector<ScheduledPhase>& schedule);

    /// Operator @p operator_name, on @p connection, asks for the trading state with a TradingStateQuery. Nothing is
    /// written: the sequencer that leads answers StateOnly, and one that does not lead answers NotLeader, each with the
    /// state as it knows it.
    void state_query(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name);

    /**
     * @brief The check once a second: enters the next phase if the schedule's time for it has come by @p now_utc_ns,
     *        nanoseconds since 1970 UTC by the leader's clock, and its conditions hold.
     *
     * When the time is no more than @p check_interval_ns away, and the phase waits on the matching engine, the engine is
     * asked how far it has got now, so that its answer is here by the check at which the time comes.
     */
    void scheduled_check(int64_t now_utc_ns, int64_t check_interval_ns = 1'000'000'000);

    /// What the matching engine orders are routed to said, in answer to EnginePositionQuery, of its reports.
    struct EngineReports {
        EngineReportPosition sent_through{}; ///< the last report it sent; number 0 when it has sent none
        int64_t held{0};                     ///< how many it holds, not yet sent to every sequencer
    };

    /// The matching engine orders are routed to has acted on every command through @p highest_applied, and said @p reports
    /// of its reports.
    void engine_position(int64_t highest_applied, const EngineReports& reports);

    /// This instance has stopped leading: what it knew of the engine's position belongs to the engine it routed to, and
    /// what it knew of the copies of the WAL is asked again by the next leader.
    void stopped_leading();

    /// The matching engine publisher on @p connection says its copy of the WAL holds every record up to @p last_seq_no
    /// (WalHeldReply).
    void publisher_holds(const pubsub_itc_fw::ConnectionID& connection, int64_t last_seq_no);

    /// The connection to the matching engine publisher on @p connection has gone.
    void publisher_gone(const pubsub_itc_fw::ConnectionID& connection);

    /// The highest command the leading engine has said it acted on, since this instance last started leading.
    [[nodiscard]] int64_t engine_applied_through() const {
        return engine_applied_through_;
    }

  private:
    // What an attempt to enter a phase, or to write a schedule, came to.
    struct Attempt {
        pubsub_itc_fw_app::TradingPhaseOutcome outcome{pubsub_itc_fw_app::TradingPhaseOutcome::NotWritten};
        int64_t technical_event_seq_no{0};
        int64_t schedule_seq_no{0};
        std::string explanation;
    };

    // Answers @p attempt, logging the answer at @p level: Warning for a request that may change the venue, Info for a question.
    void answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, const Attempt& attempt,
                pubsub_itc_fw::FwLogLevel level = pubsub_itc_fw::FwLogLevel::Warning);
    void answer_schedule(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::TradingScheduleOutcome outcome,
                         int64_t schedule_seq_no, const std::string& explanation);
    Attempt start_of_day(std::string_view operator_name, int32_t trading_day, const std::vector<ScheduledPhase>& schedule);
    // Writes the schedule for the day that has started, after the start of day. @return its position, or zero if it was
    // not written.
    int64_t write_schedule(std::string_view operator_name, int32_t trading_day, const std::vector<ScheduledPhase>& schedule);
    // Enters @p phase, any phase after the start of day, if it is the next one and its conditions hold, giving @p reason
    // as the cause.
    Attempt move_on(pubsub_itc_fw_app::TradingPhase phase, pubsub_itc_fw_app::TechnicalEventReason reason);
    // Asks the matching engine how far it has got, if @p phase waits on it and it has not yet said it has acted on what
    // the phase waits for.
    void ask_engine_ahead_of(pubsub_itc_fw_app::TradingPhase phase);
    // Why the engine's reports are not yet all in the WAL, as the latest answer gave them; empty when they are.
    [[nodiscard]] std::string reports_outstanding() const;
    // In the end-of-day begin: writes the end-of-day end once every copy of the WAL holds the begin, or the limit of time
    // has passed, and asks the publishers how far their copies go until then.
    void end_of_day_end_check(int64_t now_utc_ns);
    // In the end-of-day end: logs, once, that the follower sequencer has acknowledged the end-of-day end.
    void note_the_follower_holds_the_end();
    // Writes the TechnicalEvent entering @p phase, carrying whether trading is halted. @return its position, or zero if it
    // was not written.
    int64_t write_phase(pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day, pubsub_itc_fw_app::TechnicalEventReason reason);

    TradingPhaseRequestsLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;
    int64_t engine_applied_through_{0};
    EngineReports engine_reports_{};
    // Why the last scheduled check that found a phase due did not enter it, as it was logged at Warning; a check that finds
    // the same reason logs it at Debug, so a phase waiting on its conditions is reported once, not every second.
    std::string scheduled_check_waiting_;
    EndOfDayCopies copies_;
    // The state of the copies as last logged at Info while waiting, so that each change is logged once.
    std::string copies_last_logged_;
    // The end-of-day end whose acknowledgement by the follower sequencer has been logged; zero while none has.
    int64_t end_acknowledgement_logged_for_{0};
};

} // namespaces
