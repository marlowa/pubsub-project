// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "StalledEngineRule.hpp"

namespace {

using sequencer::StalledEngineRule;
using Position = StalledEngineRule::Position;
using Verdict = StalledEngineRule::Verdict;

constexpr int64_t second = 1'000'000'000;
constexpr std::chrono::seconds limit{3};
constexpr bool nothing_unread = false;
constexpr bool unread_waiting = true;

// Starts the round at second t, having sent the engine commands up to highest_sent, and answers its query.
Verdict round_answered(StalledEngineRule& rule, int64_t t, int64_t highest_sent, const Position& position) {
    const Verdict verdict = rule.start_round(t * second, highest_sent, nothing_unread);
    rule.answer(rule.request_id(), position);
    return verdict;
}

// Starts the round at second t and leaves its query unanswered.
Verdict round_unanswered(StalledEngineRule& rule, int64_t t, int64_t highest_sent) {
    return rule.start_round(t * second, highest_sent, nothing_unread);
}

Position applied(int64_t highest_applied) {
    return Position{highest_applied, 0, 0, 0};
}

Position sent_reports(int64_t highest_applied, int32_t epoch, int64_t through, int64_t held) {
    return Position{highest_applied, epoch, through, held};
}

TEST(StalledEngineRuleTest, AnEngineFarBehindThatKeepsApplyingIsNeverJudgedStopped) {
    StalledEngineRule rule(limit);
    int64_t applied_so_far = 0;
    for (int64_t t = 0; t < 30; ++t) {
        applied_so_far += 1000;
        // Sixty thousand commands ahead of the engine all the time, as in the load scenarios.
        EXPECT_EQ(round_answered(rule, t, applied_so_far + 60'000, applied(applied_so_far)), Verdict::working) << "at second " << t;
    }
}

TEST(StalledEngineRuleTest, AnEngineThatStopsApplyingWhileItHasCommandsIsJudgedStoppedAtTheLimitAndNotBefore) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    // From here on the engine says it has applied 10, while it has been sent more.
    EXPECT_EQ(round_answered(rule, 1, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 2, 40, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 3, 50, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 4, 60, applied(10)), Verdict::stopped_applying) << "the time ran from the round that began at second 1";
}

TEST(StalledEngineRuleTest, AnEngineWithNothingToApplyIsNeverJudgedStopped) {
    StalledEngineRule rule(limit);
    for (int64_t t = 0; t < 30; ++t) {
        EXPECT_EQ(round_answered(rule, t, 100, applied(100)), Verdict::working) << "at second " << t;
    }
}

TEST(StalledEngineRuleTest, AnEngineThatHoldsReportsAndSendsNoneIsJudgedStoppedAtTheLimit) {
    StalledEngineRule rule(limit);
    int64_t applied_so_far = 0;
    Verdict verdict = Verdict::working;
    for (int64_t t = 0; t <= 4; ++t) {
        // Still applying commands, so only the second time can run.
        applied_so_far += 10;
        verdict = round_answered(rule, t, applied_so_far, sent_reports(applied_so_far, 2, 500, 40));
        if (t < 4) {
            EXPECT_EQ(verdict, Verdict::working) << "at second " << t;
        }
    }
    EXPECT_EQ(verdict, Verdict::stopped_sending);
}

TEST(StalledEngineRuleTest, AnEngineHoldingNoReportsIsNotJudgedForSendingNone) {
    StalledEngineRule rule(limit);
    for (int64_t t = 0; t < 10; ++t) {
        EXPECT_EQ(round_answered(rule, t, 100, sent_reports(100, 2, 500, 0)), Verdict::working) << "at second " << t;
    }
}

TEST(StalledEngineRuleTest, AReportNumberThatStartsAgainUnderANewEpochCountsAsMovement) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 100, sent_reports(100, 2, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 1, 100, sent_reports(100, 2, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 2, 100, sent_reports(100, 2, 500, 40)), Verdict::working);
    // Under a new epoch the numbering starts again, and has reached 500 again: the same number, and still movement.
    EXPECT_EQ(round_answered(rule, 3, 100, sent_reports(100, 3, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 4, 100, sent_reports(100, 3, 500, 40)), Verdict::working) << "the time started again at the change of epoch";
}

TEST(StalledEngineRuleTest, AnUnansweredQueryCountsAsNoMovement) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    EXPECT_EQ(round_unanswered(rule, 1, 30), Verdict::working);
    EXPECT_EQ(round_unanswered(rule, 2, 30), Verdict::working);
    EXPECT_EQ(round_unanswered(rule, 3, 30), Verdict::working);
    EXPECT_EQ(round_unanswered(rule, 4, 30), Verdict::stopped_applying);
}

TEST(StalledEngineRuleTest, AnAnswerToAnEarlierQueryIsIgnored) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    int64_t earlier_request = rule.request_id();
    for (int64_t t = 1; t <= 4; ++t) {
        const Verdict verdict = round_unanswered(rule, t, 30);
        // Each round, the answer to the query before it arrives, late, and shows the engine moving.
        rule.answer(earlier_request, applied(10 + t));
        earlier_request = rule.request_id();
        if (t < 4) {
            EXPECT_EQ(verdict, Verdict::working) << "at second " << t;
        } else {
            EXPECT_EQ(verdict, Verdict::stopped_applying) << "late answers must not count as movement";
        }
    }
}

TEST(StalledEngineRuleTest, NeitherTimeRunsWhileTheSequencerHasUnreadMessagesFromTheEngine) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, sent_reports(10, 2, 500, 40)), Verdict::working);
    for (int64_t t = 1; t < 10; ++t) {
        // The engine has stopped moving, but only because the sequencer is not reading what it sent.
        EXPECT_EQ(rule.start_round(t * second, 20 + t, unread_waiting), Verdict::working) << "at second " << t;
        rule.answer(rule.request_id(), sent_reports(10, 2, 500, 40));
    }
    // The sequencer is first seen caught up at second 10, and the times start from then, so the engine is judged
    // stopped a full limit later, at second 13.
    EXPECT_EQ(round_answered(rule, 10, 30, sent_reports(10, 2, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 11, 30, sent_reports(10, 2, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 12, 30, sent_reports(10, 2, 500, 40)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 13, 30, sent_reports(10, 2, 500, 40)), Verdict::stopped_applying);
}

TEST(StalledEngineRuleTest, RestartingForgetsTheEngine) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 1, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 2, 40, applied(10)), Verdict::working);
    rule.restart();
    // Without the restart the engine would be judged stopped at second 4. After it, the answer to the round that opens
    // at second 3 is a new baseline, so the engine is judged as one first seen then: stopped four rounds on, at second 7.
    EXPECT_EQ(round_answered(rule, 3, 50, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 4, 50, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 5, 50, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 6, 50, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 7, 50, applied(10)), Verdict::stopped_applying);
}

TEST(StalledEngineRuleTest, TheLongestTimeWithoutProgressIsRecorded) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 1, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 2, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 3, 30, applied(30)), Verdict::working);
    // The time ran from the round that began at second 1 until the movement was seen at second 4.
    EXPECT_EQ(round_answered(rule, 4, 30, applied(30)), Verdict::working);
    EXPECT_EQ(rule.longest_without_progress_ns(), 3 * second);
    EXPECT_EQ(round_answered(rule, 5, 40, applied(30)), Verdict::working);
    EXPECT_EQ(rule.longest_without_progress_ns(), 3 * second) << "a shorter pause does not replace the longest";
}

} // namespaces
