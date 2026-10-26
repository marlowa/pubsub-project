// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "EngineAbsenceRule.hpp"
#include "EngineRoutedStamp.hpp"

namespace {

using sequencer::EngineAbsenceRule;
using sequencer::EngineRoutedStamp;

constexpr int64_t second = 1'000'000'000;
constexpr int64_t start = 1'792'900'000 * second;
constexpr std::chrono::seconds limit{5};
constexpr bool leads = true;
constexpr bool follows = false;
constexpr bool wal_has_records = true;
constexpr bool wal_is_empty = false;
constexpr bool engine = true;
constexpr bool no_engine = false;

// While running: an engine is routed to, then none is, and the command is due only once the limit has passed.
TEST(EngineAbsenceRuleTest, TheCommandIsDueOnlyWhenNoEngineHasBeenRoutedToForLongerThanTheLimit) {
    EngineAbsenceRule rule(limit);
    rule.started(0, 0);
    rule.observe(engine, start);
    EXPECT_FALSE(rule.command_due(start, leads, wal_has_records));

    rule.observe(no_engine, start + second);
    EXPECT_EQ(rule.absence_began_ns(), start + second);
    rule.observe(no_engine, start + 6 * second);
    EXPECT_FALSE(rule.command_due(start + 6 * second, leads, wal_has_records)) << "exactly the limit is not longer than it";
    rule.observe(no_engine, start + 7 * second);
    EXPECT_TRUE(rule.command_due(start + 7 * second, leads, wal_has_records));
}

// An engine routed to again ends the absence, before the limit and after it.
TEST(EngineAbsenceRuleTest, AnEngineRoutedToAgainEndsTheAbsence) {
    EngineAbsenceRule rule(limit);
    rule.started(0, 0);
    rule.observe(no_engine, start);
    rule.observe(engine, start + 3 * second);
    rule.observe(no_engine, start + 4 * second);
    EXPECT_FALSE(rule.command_due(start + 8 * second, leads, wal_has_records)) << "the absence began again at 4 s, not at 0 s";
    EXPECT_TRUE(rule.command_due(start + 10 * second, leads, wal_has_records));
}

// Written once for each absence, and again for the next one.
TEST(EngineAbsenceRuleTest, TheCommandIsWrittenOnceForEachAbsence) {
    EngineAbsenceRule rule(limit);
    rule.started(0, 0);
    rule.observe(no_engine, start);
    ASSERT_TRUE(rule.command_due(start + 6 * second, leads, wal_has_records));
    rule.command_written();
    rule.observe(no_engine, start + 7 * second);
    EXPECT_FALSE(rule.command_due(start + 60 * second, leads, wal_has_records));

    rule.observe(engine, start + 61 * second);
    rule.observe(no_engine, start + 62 * second);
    EXPECT_TRUE(rule.command_due(start + 68 * second, leads, wal_has_records)) << "a second absence is a second decision";
}

// Only the leader writes it, and not into an empty WAL, which holds no order to cancel.
TEST(EngineAbsenceRuleTest, TheCommandIsDueOnlyToALeaderWhoseWalHoldsRecords) {
    EngineAbsenceRule rule(limit);
    rule.started(0, 0);
    rule.observe(no_engine, start);
    EXPECT_FALSE(rule.command_due(start + 6 * second, follows, wal_has_records));
    EXPECT_FALSE(rule.command_due(start + 6 * second, leads, wal_is_empty));
    EXPECT_TRUE(rule.command_due(start + 6 * second, leads, wal_has_records));
}

// After the whole venue was down: the absence is measured from the stamp, not from when this process started.
TEST(EngineAbsenceRuleTest, AfterTheWholeVenueWasDownTheAbsenceIsMeasuredFromTheStamp) {
    EngineAbsenceRule rule(limit);
    const int64_t first_record = start;
    const int64_t engine_last_routed = start + 100 * second;
    rule.started(engine_last_routed, first_record);
    EXPECT_EQ(rule.absence_began_ns(), engine_last_routed);
    const int64_t restarted = engine_last_routed + 10 * second;
    rule.observe(no_engine, restarted);
    EXPECT_TRUE(rule.command_due(restarted, leads, wal_has_records)) << "down for 10 s, and the limit is 5";
}

// A quiet spell before the venue went down is not absence: the stamp says an engine was routed to throughout it.
TEST(EngineAbsenceRuleTest, AQuietSpellBeforeTheVenueWentDownIsNotCountedAsAbsence) {
    EngineAbsenceRule rule(limit);
    const int64_t last_order = start;
    const int64_t went_down = start + 600 * second; // ten quiet minutes, with an engine routed to and stamping
    rule.started(went_down, last_order);
    const int64_t restarted = went_down + 2 * second;
    rule.observe(no_engine, restarted);
    EXPECT_FALSE(rule.command_due(restarted, leads, wal_has_records));
}

// A stamp older than the first record, left from before the WAL was started, says nothing about today: the absence is
// measured from the first record, before which there was nothing to cancel.
TEST(EngineAbsenceRuleTest, AStampOlderThanTheFirstRecordIsMeasuredFromTheFirstRecord) {
    EngineAbsenceRule rule(limit);
    rule.started(start - 86'400 * second, start);
    EXPECT_EQ(rule.absence_began_ns(), start);
}

// With an empty WAL nothing is measured at start-up.
TEST(EngineAbsenceRuleTest, AnEmptyWalGivesNoAbsenceAtStartUp) {
    EngineAbsenceRule rule(limit);
    rule.started(start, 0);
    EXPECT_EQ(rule.absence_began_ns(), 0);
}

// Another instance leading means the venue was not down as a whole: the start-up measure is dropped, and an absence
// counts from then.
TEST(EngineAbsenceRuleTest, AnotherInstanceLeadingDropsTheStartUpMeasure) {
    EngineAbsenceRule rule(limit);
    rule.started(start, start);
    const int64_t now = start + 3600 * second;
    rule.another_instance_leads(now);
    rule.observe(no_engine, now + second);
    EXPECT_FALSE(rule.command_due(now + 2 * second, leads, wal_has_records)) << "an hour-old stamp of this process does not describe the venue";
    EXPECT_TRUE(rule.command_due(now + 6 * second, leads, wal_has_records));
}

class EngineRoutedStampTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "engine_routed_stamp_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string dir_;
};

TEST_F(EngineRoutedStampTest, ANewStampHoldsNothingAndAWrittenOneIsReadBack) {
    {
        EngineRoutedStamp stamp;
        ASSERT_TRUE(stamp.open(dir_));
        EXPECT_EQ(stamp.stamped_at_ns(), 0);
        EXPECT_TRUE(stamp.write(start));
        EXPECT_TRUE(stamp.write(start + second));
    }
    EngineRoutedStamp reopened;
    ASSERT_TRUE(reopened.open(dir_));
    EXPECT_EQ(reopened.stamped_at_ns(), start + second);
}

TEST_F(EngineRoutedStampTest, AStampThatCannotBeOpenedHoldsNothingAndWritesNothing) {
    EngineRoutedStamp stamp;
    EXPECT_FALSE(stamp.open(dir_ + "/no_such_directory"));
    EXPECT_EQ(stamp.stamped_at_ns(), 0);
    EXPECT_FALSE(stamp.write(start));
}

} // namespaces
