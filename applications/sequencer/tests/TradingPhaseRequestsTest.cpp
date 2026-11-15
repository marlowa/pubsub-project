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
    std::vector<pubsub_itc_fw_app::TechnicalEvent> written;
    std::vector<Outcome> answers;
    pubsub_itc_fw_app::TradingPhaseAnswer last_answer{};
};

class TradingPhaseRequestsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        requests_ = std::make_unique<TradingPhaseRequests>(links_, logger_);
    }

    void ask(Phase phase, int32_t trading_day = today) {
        requests_->request(connection_, "alice", trading_day, phase);
    }

    // The venue in the start of day, with the matching engine having acted on it.
    void started_and_applied() {
        ask(Phase::StartOfDay);
        requests_->engine_position(links_.next - 1);
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
    requests_->engine_position(0);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady) << "it has not reached the start of day";
    requests_->engine_position(1);
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::Written);
}

TEST_F(TradingPhaseRequestsTest, PreOpenIsFollowedAtOnceByOpen) {
    started_and_applied();
    ask(Phase::PreOpen);
    ASSERT_EQ(links_.written.size(), 3U);
    EXPECT_EQ(links_.written[1].phase, Phase::PreOpen);
    EXPECT_EQ(links_.written[2].phase, Phase::Open);
    EXPECT_EQ(links_.last_answer.phase, Phase::Open);
    EXPECT_EQ(links_.last_answer.technical_event_seq_no, 3);
}

TEST_F(TradingPhaseRequestsTest, WithNoEngineTheVenueDoesNotMoveToPreOpen) {
    ask(Phase::StartOfDay);
    links_.engine_takes_orders = false;
    ask(Phase::PreOpen);
    EXPECT_EQ(links_.answers.back(), Outcome::NotReady);
    EXPECT_EQ(links_.engine_asked, 0) << "there is no engine to ask";
}

TEST_F(TradingPhaseRequestsTest, AskingForAPhaseAlreadyPassedWritesNothing) {
    started_and_applied();
    ask(Phase::PreOpen);
    ask(Phase::PreOpen);
    ask(Phase::StartOfDay);
    ask(Phase::Open);
    EXPECT_EQ(links_.written.size(), 3U);
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::Written, Outcome::Written, Outcome::AlreadyPast, Outcome::AlreadyPast, Outcome::AlreadyInPhase}));
}

TEST_F(TradingPhaseRequestsTest, PreOpenBeforeTheStartOfDayIsNotTheNextPhase) {
    ask(Phase::PreOpen);
    EXPECT_TRUE(links_.written.empty());
    EXPECT_EQ(links_.answers, (std::vector<Outcome>{Outcome::NotNext}));
}

TEST_F(TradingPhaseRequestsTest, ClosingIsNotYetSupported) {
    started_and_applied();
    ask(Phase::Closed);
    ask(Phase::EndOfDay);
    EXPECT_EQ(links_.written.size(), 1U);
    EXPECT_EQ(links_.answers.back(), Outcome::Unsupported);
}

TEST_F(TradingPhaseRequestsTest, AHaltIsCarriedIntoTheNextPhase) {
    started_and_applied();
    links_.halt.note_technical_event(links_.next++, true, EventReason::DeclaredByOperator);
    ask(Phase::PreOpen);
    ASSERT_EQ(links_.written.size(), 3U);
    EXPECT_TRUE(links_.written[1].halted);
    EXPECT_TRUE(links_.written[2].halted);
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
