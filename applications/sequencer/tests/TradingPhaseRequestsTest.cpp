// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "TradingPhaseRequests.hpp"

namespace sequencer::tests {

namespace {

using Outcome = pubsub_itc_fw_app::TradingPhaseOutcome;
using Phase = pubsub_itc_fw_app::TradingPhase;
using EventReason = pubsub_itc_fw_app::TechnicalEventReason;
using ScheduleOutcome = pubsub_itc_fw_app::TradingScheduleOutcome;

constexpr int32_t today = 20261116;

// The facts TradingPhaseRequests decides from, and every TechnicalEvent and TradingSchedule written and answer sent, in
// order. Writing either notes it, as the sequencer notes every record it writes.
struct RecordingLinks : public TradingPhaseRequestsLinksInterface {
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] int32_t trading_day_started_for() const override {
        return started_for;
    }
    [[nodiscard]] const TradingPhaseState& trading_phase() const override {
        return phase;
    }
    [[nodiscard]] const TradingScheduleState& trading_schedule() const override {
        return schedule;
    }
    [[nodiscard]] const TradingHaltState& trading_halt() const override {
        return halt;
    }
    [[nodiscard]] int64_t last_seq_no() const override {
        return next - 1;
    }
    [[nodiscard]] int64_t next_seq_no() const override {
        return next;
    }
    [[nodiscard]] std::optional<EngineReportPosition> latest_engine_report_in_wal() const override {
        return reports_in_wal;
    }
    [[nodiscard]] bool an_engine_takes_orders() const override {
        return engine_takes_orders;
    }
    void ask_engine_position() override {
        ++engine_asked;
    }
    bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) override {
        if (cannot_write) {
            return false;
        }
        written.push_back(event);
        phase.note_technical_event(next, event.phase, event.trading_day);
        halt.note_technical_event(next, event.halted, event.reason);
        ++next;
        return true;
    }
    bool write_trading_schedule(const pubsub_itc_fw_app::TradingSchedule& record) override {
        if (cannot_write_schedule) {
            return false;
        }
        TradingScheduleState::ScheduleRecord kept{};
        kept.seq_no = next;
        kept.trading_day = record.trading_day;
        kept.operator_name = std::string(record.operator_name);
        for (const pubsub_itc_fw_app::TradingScheduleEntry& entry : record.entries) {
            kept.entries.push_back(ScheduledPhase{entry.phase, entry.earliest_utc_ns});
        }
        schedules_written.push_back(kept);
        schedule.note_schedule(kept);
        ++next;
        return true;
    }
    void answer(const pubsub_itc_fw::ConnectionID&, const pubsub_itc_fw_app::TradingPhaseAnswer& reply) override {
        answers.push_back(reply.outcome);
        last_answer = reply;
    }
    void answer(const pubsub_itc_fw::ConnectionID&, const pubsub_itc_fw_app::TradingScheduleAnswer& reply) override {
        schedule_answers.push_back(reply.outcome);
        last_schedule_answer = reply;
    }

    bool is_leading{true};
    int32_t started_for{today};
    TradingPhaseState phase;
    TradingScheduleState schedule;
    TradingHaltState halt;
    int64_t next{1};
    bool engine_takes_orders{true};
    int engine_asked{0};
    bool cannot_write{false};
    bool cannot_write_schedule{false};
    std::optional<EngineReportPosition> reports_in_wal;
    std::vector<pubsub_itc_fw_app::TechnicalEvent> written;
    std::vector<Outcome> answers;
    pubsub_itc_fw_app::TradingPhaseAnswer last_answer{};
    std::vector<TradingScheduleState::ScheduleRecord> schedules_written;
    std::vector<ScheduleOutcome> schedule_answers;
    pubsub_itc_fw_app::TradingScheduleAnswer last_schedule_answer{};
};

class TradingPhaseRequestsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        requests_ = std::make_unique<TradingPhaseRequests>(links_, logger_);
    }

    // The engine has acted on every command through @p highest_applied, and sent @p reports_sent reports under epoch 3,
    // holding @p held.
    void engine_says(int64_t highest_applied, int64_t reports_sent = 0, int64_t held = 0) {
        requests_->engine_position(highest_applied, TradingPhaseRequests::EngineReports{EngineReportPosition{reports_sent > 0 ? 3 : 0, reports_sent}, held});
    }

    void ask(Phase phase, int32_t trading_day = today) {
        requests_->request(connection_, "alice", trading_day, phase);
    }

    // Asks for the start of day with @p schedule.
    void start_with(const std::vector<ScheduledPhase>& schedule) {
        requests_->request(connection_, "alice", today, Phase::StartOfDay, schedule);
    }

    // Asks for the times that remain to be replaced by @p schedule.
    void replace_schedule(const std::vector<ScheduledPhase>& schedule, int32_t trading_day = today) {
        requests_->schedule_request(connection_, "alice", trading_day, schedule);
    }

    // The venue in the start of day, with the matching engine having acted on it.
    void started_and_applied() {
        ask(Phase::StartOfDay);
        engine_says(links_.next - 1);
    }

    // The venue open: start of day, pre-open and open, each written.
    void opened() {
        started_and_applied();
        ask(Phase::PreOpen);
        ask(Phase::Open);
    }

    // The venue closed, with the matching engine having acted on the close.
    void closed_and_applied() {
        opened();
        ask(Phase::Closed);
        engine_says(links_.next - 1);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<TradingPhaseRequests> requests_;
    const pubsub_itc_fw::ConnectionID connection_{61};
};

} // namespaces

TEST_F(TradingPhaseRequestsTest, TheStartOfDayIsWrittenAsTheFirstRecordOfAnEmptyWal) {
    ask(Phase::StartOfDay);
    ASSERT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.written[0].phase, Phase::StartOfDay);
    EXPECT_EQ(links_.written[0].trading_day, today);
    EXPECT_FALSE(links_.written[0].halted);
    EXPECT_EQ(links_.written[0].reason, EventReason::RequestedByOperator);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Written}));
    EXPECT_TRUE(links_.last_answer.day_started);
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 1);
    ASSERT_EQ(links_.schedules_written.size(), 1U) << "the schedule follows the start of day, empty for a day run by hand";
    EXPECT_TRUE(links_.schedules_written[0].entries.empty());
    EXPECT_EQ(links_.last_answer.schedule_seq_no, 2);
}

TEST_F(TradingPhaseRequestsTest, ASequencerThatDoesNotLeadWritesNothing) {
    links_.is_leading = false;
    ask(Phase::StartOfDay);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotLeader}));
}

TEST_F(TradingPhaseRequestsTest, ARequestForAnotherTradingDayIsRefused) {
    ask(Phase::StartOfDay, 20261117);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::WrongTradingDay}));
}

TEST_F(TradingPhaseRequestsTest, AWalHoldingRecordsButNoStartOfDayIsNotStarted) {
    links_.next = 6;
    ask(Phase::StartOfDay);
    EXPECT_TRUE(links_.written.empty()) << "the start of day must be the first record";
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::WrongTradingDay}));
}

TEST_F(TradingPhaseRequestsTest, AskingForTheStartOfDayAgainWritesNothing) {
    ask(Phase::StartOfDay);
    ask(Phase::StartOfDay);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Written, Outcome::AlreadyInPhase}));
}

TEST_F(TradingPhaseRequestsTest, PreOpenWaitsForTheEngineToHaveActedOnTheStartOfDay) {
    ask(Phase::StartOfDay);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
    EXPECT_EQ(links_.engine_asked, 1) << "the engine is asked how far it has got";
    engine_says(0);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "it has not reached the start of day";
    engine_says(1);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
}

TEST_F(TradingPhaseRequestsTest, EveryPhaseIsEnteredOnRequestInOrder) {
    started_and_applied();
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.last_answer.phase, Phase::PreOpen) << "open is not written until it is asked for";
    ask(Phase::Open);
    ask(Phase::Closed);
    engine_says(links_.next - 1);
    ask(Phase::EndOfDay);
    ASSERT_EQ(links_.written.size(), 5U);
    const std::vector<Phase> wanted{Phase::StartOfDay, Phase::PreOpen, Phase::Open, Phase::Closed, Phase::EndOfDay};
    for (size_t index = 0; index < wanted.size(); ++index) {
        EXPECT_EQ(links_.written[index].phase, wanted[index]) << "record " << index + 1;
        EXPECT_EQ(links_.written[index].trading_day, today);
    }
    EXPECT_EQ(links_.answers, (std::vector<Outcome>(5, Outcome::Written)));
    EXPECT_EQ(links_.last_answer.phase, Phase::EndOfDay);
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 6) << "the start of day, its schedule, and four phases";
}

TEST_F(TradingPhaseRequestsTest, APhaseFurtherAheadThanTheNextIsRefused) {
    started_and_applied();
    ask(Phase::Open);
    ask(Phase::Closed);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Written, Outcome::NotNext, Outcome::NotNext, Outcome::NotNext}));
}

TEST_F(TradingPhaseRequestsTest, TheEndOfDayWaitsForTheEngineToHaveActedOnTheClose) {
    opened();
    ask(Phase::Closed);
    const int64_t close_seq_no = links_.next - 1;
    const int asked_before = links_.engine_asked;
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
    EXPECT_EQ(links_.engine_asked, asked_before + 1) << "the engine is asked how far it has got";
    engine_says(close_seq_no - 1);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "it has not reached the close";
    engine_says(close_seq_no);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
    EXPECT_EQ(links_.written.back().phase, Phase::EndOfDay);
}

// The engine's answer can arrive before the close's reports, which travel on other connections: the end of day waits
// until the WAL holds every report the engine says it sent, and until it holds none unsent.
TEST_F(TradingPhaseRequestsTest, TheEndOfDayWaitsForEveryReportTheEngineSentToBeInTheWal) {
    opened();
    ask(Phase::Closed);
    const int64_t close_seq_no = links_.next - 1;
    engine_says(close_seq_no, 7);
    links_.reports_in_wal = EngineReportPosition{3, 5};
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "reports 6 and 7 are not yet in the WAL";
    links_.reports_in_wal = EngineReportPosition{3, 7};
    engine_says(close_seq_no, 7, 2);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "the engine holds reports not yet sent";
    engine_says(close_seq_no, 7);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
}

// Reports numbered under a later epoch of the engine come after every report of an earlier one.
TEST_F(TradingPhaseRequestsTest, AReportInTheWalUnderALaterEpochCoversEveryEarlierOne) {
    opened();
    ask(Phase::Closed);
    engine_says(links_.next - 1, 7);
    links_.reports_in_wal = EngineReportPosition{4, 1};
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
}

TEST_F(TradingPhaseRequestsTest, WithNoEngineTheVenueDoesNotMoveToTheEndOfDay) {
    opened();
    ask(Phase::Closed);
    links_.engine_takes_orders = false;
    const int asked_before = links_.engine_asked;
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
    EXPECT_EQ(links_.engine_asked, asked_before) << "there is no engine to ask";
}

TEST_F(TradingPhaseRequestsTest, WithNoEngineTheVenueDoesNotMoveToPreOpen) {
    ask(Phase::StartOfDay);
    links_.engine_takes_orders = false;
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
    EXPECT_EQ(links_.engine_asked, 0) << "there is no engine to ask";
}

TEST_F(TradingPhaseRequestsTest, AskingForAPhaseAlreadyPassedWritesNothing) {
    closed_and_applied();
    const size_t written_before = links_.written.size();
    links_.answers.clear();
    ask(Phase::StartOfDay);
    ask(Phase::PreOpen);
    ask(Phase::Open);
    ask(Phase::Closed);
    EXPECT_EQ(links_.written.size(), written_before);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::AlreadyPast, Outcome::AlreadyPast, Outcome::AlreadyPast, Outcome::AlreadyInPhase}));
}

TEST_F(TradingPhaseRequestsTest, PreOpenBeforeTheStartOfDayIsNotTheNextPhase) {
    ask(Phase::PreOpen);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotNext}));
}

TEST_F(TradingPhaseRequestsTest, TheVenueClosesWhileHaltedByAnOperator) {
    opened();
    links_.halt.note_technical_event(links_.next++, true, EventReason::DeclaredByOperator);
    ask(Phase::Closed);
    EXPECT_EQ(links_.answers.back(), Outcome::Written) << "a halt must not stop the close from cancelling the open orders";
    EXPECT_EQ(links_.written.back().phase, Phase::Closed);
    EXPECT_TRUE(links_.written.back().halted);
}

TEST_F(TradingPhaseRequestsTest, TheVenueDoesNotCloseWhileTheOrdersCannotBeAccountedFor) {
    opened();
    links_.halt.note_technical_event(links_.next++, true, EventReason::CannotAccountForBook);
    const size_t written_before = links_.written.size();
    ask(Phase::Closed);
    EXPECT_EQ(links_.written.size(), written_before);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
}

TEST_F(TradingPhaseRequestsTest, AHaltIsCarriedIntoTheNextPhase) {
    started_and_applied();
    links_.halt.note_technical_event(links_.next++, true, EventReason::DeclaredByOperator);
    ask(Phase::PreOpen);
    ASSERT_EQ(links_.written.size(), 2U);
    EXPECT_TRUE(links_.written[1].halted);
    EXPECT_TRUE(links_.halt.halted());
    EXPECT_EQ(links_.halt.latest()->reason, EventReason::DeclaredByOperator);
}

TEST_F(TradingPhaseRequestsTest, AHaltBecauseTheOrdersCannotBeAccountedForHoldsThePhase) {
    started_and_applied();
    links_.halt.note_technical_event(links_.next++, true, EventReason::CannotAccountForBook);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
}

TEST_F(TradingPhaseRequestsTest, WhatWasKnownOfTheEngineIsForgottenOnLosingTheLead) {
    started_and_applied();
    requests_->stopped_leading();
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "the next engine is asked afresh";
}

TEST_F(TradingPhaseRequestsTest, AnEventThatCannotBeWrittenIsSaidSo) {
    links_.cannot_write = true;
    ask(Phase::StartOfDay);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotWritten}));
    EXPECT_FALSE(links_.last_answer.day_started);
}

// ---- The day's schedule ---------------------------------------------------------------------------------------------

TEST_F(TradingPhaseRequestsTest, TheScheduleSentWithTheStartOfDayIsWrittenStraightAfterIt) {
    start_with({{Phase::PreOpen, 1000}, {Phase::Open, 2000}});
    ASSERT_EQ(links_.written.size(), 1U);
    ASSERT_EQ(links_.schedules_written.size(), 1U);
    const TradingScheduleState::ScheduleRecord& schedule = links_.schedules_written[0];
    EXPECT_EQ(schedule.seq_no, 2);
    EXPECT_EQ(schedule.trading_day, today);
    EXPECT_EQ(schedule.operator_name, "alice");
    EXPECT_EQ(schedule.entries, (std::vector<ScheduledPhase>{{Phase::PreOpen, 1000}, {Phase::Open, 2000}}));
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Written}));
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 1);
    EXPECT_EQ(links_.last_answer.schedule_seq_no, 2);
}

TEST_F(TradingPhaseRequestsTest, AScheduleTheVenueCannotFollowIsRefusedAndNothingIsWritten) {
    start_with({{Phase::Open, 1000}});
    start_with({{Phase::PreOpen, 2000}, {Phase::Open, 1000}});
    start_with({{Phase::PreOpen, 1000}, {Phase::Closed, 2000}});
    start_with({{Phase::PreOpen, -1}});
    EXPECT_TRUE(links_.written.empty());
    EXPECT_TRUE(links_.schedules_written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>(4, Outcome::ScheduleRefused)));
    EXPECT_FALSE(links_.last_answer.day_started);
}

TEST_F(TradingPhaseRequestsTest, AScheduleCheckAllowsAsSoonAsPossibleAfterATime) {
    EXPECT_EQ(schedule_problem(Phase::StartOfDay, {{Phase::PreOpen, 1000}, {Phase::Open, 0}, {Phase::Closed, 3000}, {Phase::EndOfDay, 0}}), "");
    EXPECT_EQ(schedule_problem(Phase::StartOfDay, {}), "") << "a day run entirely by hand";
    EXPECT_EQ(schedule_problem(Phase::Open, {{Phase::Closed, 3000}}), "");
    EXPECT_NE(schedule_problem(Phase::Open, {{Phase::Open, 3000}}), "") << "a schedule never names the phase the venue is in";
    EXPECT_NE(schedule_problem(Phase::Open, {{Phase::EndOfDay, 3000}}), "") << "nor skips one";
    EXPECT_NE(schedule_problem(Phase::StartOfDay, {{Phase::PreOpen, 3000}, {Phase::Open, 0}, {Phase::Closed, 2000}}), "")
        << "a time after an as-soon-as-possible entry is still compared with the times before it";
}

// The leader stopped between writing the start of day and the schedule: the request sent again writes the schedule alone.
TEST_F(TradingPhaseRequestsTest, AStartOfDayWithoutItsScheduleIsGivenItByTheRequestSentAgain) {
    links_.cannot_write_schedule = true;
    start_with({{Phase::PreOpen, 1000}});
    EXPECT_EQ(links_.answers.back(), Outcome::NotWritten);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_TRUE(links_.schedules_written.empty());
    links_.cannot_write_schedule = false;
    start_with({{Phase::PreOpen, 1000}});
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
    EXPECT_EQ(links_.written.size(), 1U) << "the start of day is not written twice";
    ASSERT_EQ(links_.schedules_written.size(), 1U);
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 0);
    EXPECT_EQ(links_.last_answer.schedule_seq_no, 2);
    start_with({{Phase::PreOpen, 1000}});
    EXPECT_EQ(links_.answers.back(), Outcome::AlreadyInPhase);
    EXPECT_EQ(links_.schedules_written.size(), 1U) << "once a schedule is recorded, the start-of-day request does not replace it";
}

TEST_F(TradingPhaseRequestsTest, NoPhaseIsEnteredBeforeItsTime) {
    start_with({{Phase::PreOpen, 1000}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(999);
    EXPECT_EQ(links_.written.size(), 1U);
    requests_->scheduled_check(1000);
    ASSERT_EQ(links_.written.size(), 2U);
    EXPECT_EQ(links_.written[1].phase, Phase::PreOpen);
    EXPECT_EQ(links_.written[1].reason, EventReason::Scheduled);
}

TEST_F(TradingPhaseRequestsTest, TheEngineIsAskedOneCheckAheadSoThePhaseIsEnteredAtTheCheckItsTimeComes) {
    start_with({{Phase::PreOpen, 5000}});
    requests_->scheduled_check(3000, 1000);
    EXPECT_EQ(links_.engine_asked, 0) << "more than one check ahead";
    requests_->scheduled_check(4000, 1000);
    EXPECT_EQ(links_.engine_asked, 1) << "one check ahead";
    engine_says(1);
    requests_->scheduled_check(5000, 1000);
    ASSERT_EQ(links_.written.size(), 2U) << "entered at the check its time comes";
    EXPECT_EQ(links_.written[1].phase, Phase::PreOpen);
}

TEST_F(TradingPhaseRequestsTest, AScheduledPhaseWaitsForItsConditions) {
    start_with({{Phase::PreOpen, 1000}});
    requests_->scheduled_check(2000);
    EXPECT_EQ(links_.written.size(), 1U) << "the engine has not said it acted on the start of day";
    EXPECT_EQ(links_.engine_asked, 1);
    engine_says(1);
    requests_->scheduled_check(2001);
    ASSERT_EQ(links_.written.size(), 2U);
    EXPECT_EQ(links_.written[1].phase, Phase::PreOpen);
}

TEST_F(TradingPhaseRequestsTest, AWaitingPhaseIsReportedOnceAtWarningNotEverySecond) {
    start_with({{Phase::PreOpen, 0}});
    links_.engine_takes_orders = false;
    log_.clear();
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    requests_->scheduled_check(3);
    logger_.flush();
    size_t reported = 0;
    for (const std::string& line : log_) {
        reported += line.find("was not entered") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(reported, 1U);
}

TEST_F(TradingPhaseRequestsTest, TheScheduleTakesTheVenueThroughEveryPhase) {
    start_with({{Phase::PreOpen, 0}, {Phase::Open, 0}, {Phase::Closed, 5000}, {Phase::EndOfDay, 0}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    EXPECT_EQ(links_.written.size(), 3U) << "one phase for each check";
    requests_->scheduled_check(4999);
    EXPECT_EQ(links_.written.size(), 3U);
    requests_->scheduled_check(5000);
    EXPECT_EQ(links_.written.size(), 4U);
    requests_->scheduled_check(5001);
    EXPECT_EQ(links_.written.size(), 4U) << "the end of day waits for the engine to have acted on the close";
    engine_says(links_.next - 1);
    requests_->scheduled_check(5002);
    ASSERT_EQ(links_.written.size(), 5U);
    const std::vector<Phase> wanted{Phase::StartOfDay, Phase::PreOpen, Phase::Open, Phase::Closed, Phase::EndOfDay};
    for (size_t index = 0; index < wanted.size(); ++index) {
        EXPECT_EQ(links_.written[index].phase, wanted[index]) << "record " << index + 1;
        EXPECT_EQ(links_.written[index].reason, index == 0 ? EventReason::RequestedByOperator : EventReason::Scheduled) << "record " << index + 1;
    }
    requests_->scheduled_check(9000);
    EXPECT_EQ(links_.written.size(), 5U) << "nothing follows the end of day";
}

TEST_F(TradingPhaseRequestsTest, APhaseTheScheduleLeavesOutIsEnteredOnlyOnRequest) {
    start_with({{Phase::PreOpen, 0}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(1'000'000);
    EXPECT_EQ(links_.written.size(), 2U);
    EXPECT_EQ(links_.written.back().phase, Phase::PreOpen);
    ask(Phase::Open);
    EXPECT_EQ(links_.written.back().phase, Phase::Open);
}

TEST_F(TradingPhaseRequestsTest, ASequencerThatDoesNotLeadDoesNotFollowTheSchedule) {
    start_with({{Phase::PreOpen, 0}});
    engine_says(links_.next - 1);
    links_.is_leading = false;
    requests_->scheduled_check(1);
    EXPECT_EQ(links_.written.size(), 1U);
}

TEST_F(TradingPhaseRequestsTest, ARequestAndTheScheduleTogetherEnterAPhaseOnce) {
    start_with({{Phase::PreOpen, 0}});
    engine_says(links_.next - 1);
    ask(Phase::PreOpen);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    EXPECT_EQ(links_.written.size(), 2U);
    EXPECT_EQ(links_.written.back().reason, EventReason::RequestedByOperator);
}

TEST_F(TradingPhaseRequestsTest, TheScheduleClosesTheVenueWhileHaltedByAnOperator) {
    start_with({{Phase::PreOpen, 0}, {Phase::Open, 0}, {Phase::Closed, 100}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    links_.halt.note_technical_event(links_.next++, true, EventReason::DeclaredByOperator);
    requests_->scheduled_check(100);
    EXPECT_EQ(links_.written.back().phase, Phase::Closed);
    EXPECT_TRUE(links_.written.back().halted);
}

TEST_F(TradingPhaseRequestsTest, TheScheduleDoesNotCloseTheVenueWhileTheOrdersCannotBeAccountedFor) {
    start_with({{Phase::PreOpen, 0}, {Phase::Open, 0}, {Phase::Closed, 100}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    links_.halt.note_technical_event(links_.next++, true, EventReason::CannotAccountForBook);
    requests_->scheduled_check(100);
    EXPECT_EQ(links_.written.back().phase, Phase::Open);
}

TEST_F(TradingPhaseRequestsTest, AReplacedScheduleTakesEffect) {
    start_with({{Phase::PreOpen, 0}, {Phase::Open, 0}, {Phase::Closed, 5000}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    replace_schedule({{Phase::Closed, 3000}, {Phase::EndOfDay, 0}});
    EXPECT_EQ(links_.schedule_answers, (std::vector<ScheduleOutcome>{ScheduleOutcome::Written}));
    ASSERT_EQ(links_.schedules_written.size(), 2U);
    EXPECT_EQ(links_.last_schedule_answer.schedule_seq_no, links_.schedules_written[1].seq_no);
    requests_->scheduled_check(3000);
    EXPECT_EQ(links_.written.back().phase, Phase::Closed) << "the early close";
}

TEST_F(TradingPhaseRequestsTest, AnEmptyReplacementLeavesTheRestOfTheDayToTheOperator) {
    start_with({{Phase::PreOpen, 0}, {Phase::Open, 0}, {Phase::Closed, 5000}});
    engine_says(links_.next - 1);
    requests_->scheduled_check(1);
    requests_->scheduled_check(2);
    replace_schedule({});
    EXPECT_EQ(links_.schedule_answers.back(), ScheduleOutcome::Written);
    requests_->scheduled_check(6000);
    EXPECT_EQ(links_.written.back().phase, Phase::Open);
}

TEST_F(TradingPhaseRequestsTest, AReplacementNamingAPhaseEnteredOrPassedIsRefused) {
    opened();
    const size_t schedules_before = links_.schedules_written.size();
    replace_schedule({{Phase::Open, 3000}, {Phase::Closed, 4000}});
    replace_schedule({{Phase::PreOpen, 3000}});
    replace_schedule({{Phase::EndOfDay, 3000}});
    EXPECT_EQ(links_.schedule_answers, (std::vector<ScheduleOutcome>(3, ScheduleOutcome::Refused)));
    EXPECT_EQ(links_.schedules_written.size(), schedules_before);
}

TEST_F(TradingPhaseRequestsTest, AReplacementIsRefusedByAFollowerBeforeTheDayAndForAnotherDay) {
    replace_schedule({{Phase::PreOpen, 3000}});
    ask(Phase::StartOfDay);
    replace_schedule({{Phase::PreOpen, 3000}}, 20261117);
    links_.is_leading = false;
    replace_schedule({{Phase::PreOpen, 3000}});
    EXPECT_EQ(links_.schedule_answers,
              (std::vector<ScheduleOutcome>{ScheduleOutcome::TradingDayNotStarted, ScheduleOutcome::WrongTradingDay, ScheduleOutcome::NotLeader}));
    EXPECT_EQ(links_.schedules_written.size(), 1U) << "only the start of day's own";
}

TEST_F(TradingPhaseRequestsTest, AScheduleThatCannotBeWrittenIsSaidSo) {
    ask(Phase::StartOfDay);
    links_.cannot_write_schedule = true;
    replace_schedule({{Phase::PreOpen, 3000}});
    EXPECT_EQ(links_.schedule_answers, (std::vector<ScheduleOutcome>{ScheduleOutcome::NotWritten}));
}

} // namespaces
