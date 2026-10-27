// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "RecoveryLimitRule.hpp"

namespace {

using sequencer::RecoveryLimitRule;
using Progress = RecoveryLimitRule::CatchUpProgress;

constexpr int64_t second = 1'000'000'000;
constexpr int64_t start = 1'792'900'000 * second;
constexpr std::chrono::seconds limit{10};
constexpr bool leads = true;
constexpr bool follows = false;
constexpr bool wal_has_records = true;
constexpr bool wal_is_empty = false;
constexpr int64_t no_absence = 0;
const std::optional<Progress> no_catch_up = std::nullopt;

// With no catch-up to estimate from, the halt is due once the time without an engine passes the limit, and not before.
TEST(RecoveryLimitRuleTest, WithNoCatchUpTheHaltIsDueOnlyOnceTheAbsencePassesTheLimit) {
    RecoveryLimitRule rule(limit);
    EXPECT_FALSE(rule.halt_due(start + 10 * second, start, leads, wal_has_records, no_catch_up)) << "exactly the limit is not longer than it";
    EXPECT_TRUE(rule.halt_due(start + 11 * second, start, leads, wal_has_records, no_catch_up));
}

// A catch-up that will plainly not finish within the limit halts trading as soon as that is known, a second into it.
TEST(RecoveryLimitRuleTest, ACatchUpThatWillNotFinishWithinTheLimitIsHaltedASecondIn) {
    RecoveryLimitRule rule(limit);
    // 1,000,000 records to send, at 50,000 a second: 20 seconds.
    EXPECT_FALSE(rule.halt_due(start + 1 * second, start, leads, wal_has_records, Progress{0, 1'000'000}));
    EXPECT_TRUE(rule.halt_due(start + 2 * second, start, leads, wal_has_records, Progress{50'000, 1'000'000}));
    ASSERT_TRUE(rule.estimate_ns().has_value());
    EXPECT_EQ(*rule.estimate_ns(), 19 * second);
}

// One that will finish within the limit is left to finish.
TEST(RecoveryLimitRuleTest, ACatchUpThatWillFinishWithinTheLimitIsLeftToFinish) {
    RecoveryLimitRule rule(limit);
    // 300,000 records at 100,000 a second: 3 seconds.
    EXPECT_FALSE(rule.halt_due(start + 1 * second, start, leads, wal_has_records, Progress{0, 300'000}));
    EXPECT_FALSE(rule.halt_due(start + 2 * second, start, leads, wal_has_records, Progress{100'000, 300'000}));
    EXPECT_FALSE(rule.halt_due(start + 3 * second, start, leads, wal_has_records, Progress{200'000, 300'000}));
}

// The rate is measured over at least a second, so a sample taken sooner does not count.
TEST(RecoveryLimitRuleTest, TheRateIsMeasuredOverAtLeastASecond) {
    RecoveryLimitRule rule(limit);
    EXPECT_FALSE(rule.halt_due(start + 1 * second, start, leads, wal_has_records, Progress{0, 1'000'000}));
    EXPECT_FALSE(rule.halt_due(start + 1 * second + second / 2, start, leads, wal_has_records, Progress{1, 1'000'000}));
    EXPECT_FALSE(rule.estimate_ns().has_value()) << "half a second is too short to measure";
}

// A catch-up that has stopped moving gives no estimate, and the time so far alone decides.
TEST(RecoveryLimitRuleTest, ACatchUpThatDoesNotMoveGivesNoEstimate) {
    RecoveryLimitRule rule(limit);
    EXPECT_FALSE(rule.halt_due(start + 1 * second, start, leads, wal_has_records, Progress{500, 1'000'000}));
    EXPECT_FALSE(rule.halt_due(start + 2 * second, start, leads, wal_has_records, Progress{500, 1'000'000}));
    EXPECT_FALSE(rule.estimate_ns().has_value());
    EXPECT_TRUE(rule.halt_due(start + 11 * second, start, leads, wal_has_records, Progress{500, 1'000'000}));
}

// Only the leader writes commands, and an empty WAL holds nothing to recover.
TEST(RecoveryLimitRuleTest, TheHaltIsDueOnlyToALeaderWhoseWalHoldsRecords) {
    RecoveryLimitRule rule(limit);
    EXPECT_FALSE(rule.halt_due(start + 11 * second, start, follows, wal_has_records, no_catch_up));
    EXPECT_FALSE(rule.halt_due(start + 11 * second, start, leads, wal_is_empty, no_catch_up));
    EXPECT_TRUE(rule.halt_due(start + 11 * second, start, leads, wal_has_records, no_catch_up));
}

// Written once for each absence, and again for the next one.
TEST(RecoveryLimitRuleTest, TheHaltIsWrittenOnceForEachAbsence) {
    RecoveryLimitRule rule(limit);
    ASSERT_TRUE(rule.halt_due(start + 11 * second, start, leads, wal_has_records, no_catch_up));
    rule.halt_written();
    EXPECT_FALSE(rule.halt_due(start + 60 * second, start, leads, wal_has_records, no_catch_up));

    EXPECT_FALSE(rule.halt_due(start + 61 * second, no_absence, leads, wal_has_records, no_catch_up)) << "an engine is routed to again";
    EXPECT_TRUE(rule.halt_due(start + 80 * second, start + 62 * second, leads, wal_has_records, no_catch_up)) << "a second absence is a second decision";
}

// A new catch-up that starts lower than the last sample is measured afresh, not as going backwards.
TEST(RecoveryLimitRuleTest, ANewCatchUpIsMeasuredAfresh) {
    RecoveryLimitRule rule(limit);
    EXPECT_FALSE(rule.halt_due(start + 1 * second, start, leads, wal_has_records, Progress{400'000, 1'000'000}));
    EXPECT_FALSE(rule.halt_due(start + 2 * second, start, leads, wal_has_records, Progress{10, 1'000'000}));
    EXPECT_FALSE(rule.estimate_ns().has_value());
}

} // namespaces
