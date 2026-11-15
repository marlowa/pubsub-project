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

constexpr int32_t today = 20261116;

// The facts TradingPhaseRequests decides from, and every TechnicalEvent written and answer sent, in order. Writing a
// TechnicalEvent notes it, as the sequencer notes every record it writes.
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
    void answer(const pubsub_itc_fw::ConnectionID&, const pubsub_itc_fw_app::TradingPhaseAnswer& reply) override {
        answers.push_back(reply.outcome);
        last_answer = reply;
    }

    bool is_leading{true};
    int32_t started_for{today};
    TradingPhaseState phase;
    TradingHaltState halt;
    int64_t next{1};
    bool engine_takes_orders{true};
    int engine_asked{0};
    bool cannot_write{false};
    std::optional<EngineReportPosition> reports_in_wal;
    std::vector<pubsub_itc_fw_app::TechnicalEvent> written;
    std::vector<Outcome> answers;
    pubsub_itc_fw_app::TradingPhaseAnswer last_answer{};
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
    engine_says(4);
    ask(Phase::EndOfDay);
    ASSERT_EQ(links_.written.size(), 5U);
    const std::vector<Phase> wanted{Phase::StartOfDay, Phase::PreOpen, Phase::Open, Phase::Closed, Phase::EndOfDay};
    for (size_t index = 0; index < wanted.size(); ++index) {
        EXPECT_EQ(links_.written[index].phase, wanted[index]) << "record " << index + 1;
        EXPECT_EQ(links_.written[index].trading_day, today);
    }
    EXPECT_EQ(links_.answers, (std::vector<Outcome>(5, Outcome::Written)));
    EXPECT_EQ(links_.last_answer.phase, Phase::EndOfDay);
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 5);
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

} // namespaces
