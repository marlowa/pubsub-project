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
const StalledEngineRule::ReportReceived no_report_received{};

// Starts the round at second t, having sent the engine commands up to highest_sent, and answers its query.
Verdict round_answered(StalledEngineRule& rule, int64_t t, int64_t highest_sent, const Position& position) {
    const Verdict verdict = rule.start_round(t * second, highest_sent, nothing_unread, no_report_received);
    rule.answer(rule.request_id(), position, t * second + second / 10);
    return verdict;
}

// Starts the round at second t and leaves its query unanswered.
Verdict round_unanswered(StalledEngineRule& rule, int64_t t, int64_t highest_sent) {
    return rule.start_round(t * second, highest_sent, nothing_unread, no_report_received);
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
    EXPECT_EQ(rule.without_progress_ns(4 * second), 3 * second);
}

TEST(StalledEngineRuleTest, AnEngineApplyingOneLongCommandWhileSendingItsReportsIsNeverJudgedStopped) {
    StalledEngineRule rule(limit);
    // One command, at seq=10, cancels hundreds of thousands of orders in pieces: the highest applied stays at 9 for many
    // seconds, with commands waiting behind it, while a report goes out for each order cancelled.
    int64_t reports_sent = 0;
    for (int64_t t = 0; t < 30; ++t) {
        reports_sent += 20'000;
        EXPECT_EQ(round_answered(rule, t, 50, sent_reports(9, 2, reports_sent, 4000)), Verdict::working) << "at second " << t;
    }
    // Once its reports stop as well, it is stopped.
    for (int64_t t = 30; t < 33; ++t) {
        EXPECT_EQ(round_answered(rule, t, 50, sent_reports(9, 2, reports_sent, 0)), Verdict::working) << "at second " << t;
    }
    EXPECT_EQ(round_answered(rule, 33, 50, sent_reports(9, 2, reports_sent, 0)), Verdict::stopped_applying);
}

TEST(StalledEngineRuleTest, ReportsTheSequencerReceivesFromTheEngineCountAsProgress) {
    StalledEngineRule rule(limit);
    // The engine's answers never move, as when it answers late, but its reports keep reaching the sequencer.
    for (int64_t t = 0; t < 30; ++t) {
        const Verdict verdict = rule.start_round(t * second, 50, nothing_unread, StalledEngineRule::ReportReceived{2, 1000 + t});
        rule.answer(rule.request_id(), applied(10), t * second);
        EXPECT_EQ(verdict, Verdict::working) << "at second " << t;
    }
    // Once its reports stop as well, it is stopped.
    for (int64_t t = 30; t <= 32; ++t) {
        const Verdict verdict = rule.start_round(t * second, 50, nothing_unread, StalledEngineRule::ReportReceived{2, 1029});
        rule.answer(rule.request_id(), applied(10), t * second);
        EXPECT_EQ(verdict, t < 32 ? Verdict::working : Verdict::stopped_applying) << "at second " << t;
    }
}

TEST(StalledEngineRuleTest, ARoundThatStartsLateSaysNothingAboutTheEngine) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 1, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 2, 30, applied(10)), Verdict::working);
    // The sequencer did not start a round for four seconds: it was not judging, so the times start again from here.
    EXPECT_EQ(round_answered(rule, 6, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 7, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 8, 30, applied(10)), Verdict::working);
    EXPECT_EQ(round_answered(rule, 9, 30, applied(10)), Verdict::stopped_applying) << "a full limit after the late round";
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
        rule.answer(earlier_request, applied(10 + t), t * second);
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
        EXPECT_EQ(rule.start_round(t * second, 20 + t, unread_waiting, no_report_received), Verdict::working) << "at second " << t;
        rule.answer(rule.request_id(), sent_reports(10, 2, 500, 40), t * second);
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

TEST(StalledEngineRuleTest, WhatWasKnownOfTheRoundLastJudgedIsKept) {
    StalledEngineRule rule(limit);
    EXPECT_EQ(round_answered(rule, 0, 20, sent_reports(10, 2, 500, 40)), Verdict::working);
    // The next round starts late, 3.75 seconds after the first, with its answer still unread.
    EXPECT_EQ(rule.start_round(3 * second + 3 * second / 4, 30, unread_waiting, no_report_received), Verdict::working);
    const StalledEngineRule::RoundJudged& first = rule.last_round_judged();
    EXPECT_EQ(first.started_ns, 0);
    EXPECT_EQ(first.gap_since_previous_ns, 0) << "the first round has none before it";
    EXPECT_TRUE(first.answered);
    EXPECT_EQ(first.answer_delay_ns, second / 10);
    EXPECT_EQ(first.answer.reports_sent_through, 500);
    EXPECT_EQ(first.highest_sent, 20);
    EXPECT_TRUE(first.unread_from_engine);
    EXPECT_EQ(rule.start_round(4 * second + 3 * second / 4, 30, nothing_unread, no_report_received), Verdict::working);
    const StalledEngineRule::RoundJudged& second_round = rule.last_round_judged();
    EXPECT_EQ(second_round.gap_since_previous_ns, 3 * second + 3 * second / 4);
    EXPECT_FALSE(second_round.answered);
    EXPECT_EQ(second_round.highest_sent, 30);
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
    EXPECT_EQ(rule.last_round_judged().ended_after_ns, 3 * second) << "the round that saw the movement stopped a time that had run 3 seconds";
    EXPECT_EQ(round_answered(rule, 5, 40, applied(30)), Verdict::working);
    EXPECT_EQ(rule.last_round_judged().ended_after_ns, 0) << "nothing was running to stop";
    EXPECT_EQ(rule.longest_without_progress_ns(), 3 * second) << "a shorter pause does not replace the longest";
}

} // namespaces
