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
constexpr bool engine = true;
constexpr bool no_engine = false;

// While running: an engine is routed to, then none is, and the absence is measured from the first moment none was.
TEST(EngineAbsenceRuleTest, AnAbsenceIsMeasuredFromTheFirstMomentNoEngineWasRoutedTo) {
    EngineAbsenceRule rule;
    rule.started(0, 0);
    rule.observe(engine, start);
    EXPECT_EQ(rule.absence_began_ns(), 0);

    rule.observe(no_engine, start + second);
    EXPECT_EQ(rule.absence_began_ns(), start + second);
    rule.observe(no_engine, start + 6 * second);
    EXPECT_EQ(rule.absence_began_ns(), start + second) << "a later observation of no engine does not move the start";
}

// An engine routed to again ends the absence, and the next one is measured from its own start.
TEST(EngineAbsenceRuleTest, AnEngineRoutedToAgainEndsTheAbsence) {
    EngineAbsenceRule rule;
    rule.started(0, 0);
    rule.observe(no_engine, start);
    rule.observe(engine, start + 3 * second);
    EXPECT_EQ(rule.absence_began_ns(), 0);
    rule.observe(no_engine, start + 4 * second);
    EXPECT_EQ(rule.absence_began_ns(), start + 4 * second) << "the absence began again at 4 s, not at 0 s";
}

// After the whole venue was down: the absence is measured from the stamp, not from when this process started.
TEST(EngineAbsenceRuleTest, AfterTheWholeVenueWasDownTheAbsenceIsMeasuredFromTheStamp) {
    EngineAbsenceRule rule;
    const int64_t first_record = start;
    const int64_t engine_last_routed = start + 100 * second;
    rule.started(engine_last_routed, first_record);
    EXPECT_EQ(rule.absence_began_ns(), engine_last_routed);
    rule.observe(no_engine, engine_last_routed + 10 * second);
    EXPECT_EQ(rule.absence_began_ns(), engine_last_routed);
}

// A quiet spell before the venue went down is not absence: the stamp says an engine was routed to throughout it.
TEST(EngineAbsenceRuleTest, AQuietSpellBeforeTheVenueWentDownIsNotCountedAsAbsence) {
    EngineAbsenceRule rule;
    const int64_t last_order = start;
    const int64_t went_down = start + 600 * second; // ten quiet minutes, with an engine routed to and stamping
    rule.started(went_down, last_order);
    EXPECT_EQ(rule.absence_began_ns(), went_down);
}

// A stamp older than the first record, left from before the WAL was started, says nothing about today: the absence is
// measured from the first record, before which there was nothing to recover.
TEST(EngineAbsenceRuleTest, AStampOlderThanTheFirstRecordIsMeasuredFromTheFirstRecord) {
    EngineAbsenceRule rule;
    rule.started(start - 86'400 * second, start);
    EXPECT_EQ(rule.absence_began_ns(), start);
}

// With an empty WAL nothing is measured at start-up.
TEST(EngineAbsenceRuleTest, AnEmptyWalGivesNoAbsenceAtStartUp) {
    EngineAbsenceRule rule;
    rule.started(start, 0);
    EXPECT_EQ(rule.absence_began_ns(), 0);
}

// Another instance leading means the venue was not down as a whole: the start-up measure is dropped, and an absence
// counts from then.
TEST(EngineAbsenceRuleTest, AnotherInstanceLeadingDropsTheStartUpMeasure) {
    EngineAbsenceRule rule;
    rule.started(start, start);
    const int64_t now = start + 3600 * second;
    rule.another_instance_leads(now);
    rule.observe(no_engine, now + second);
    EXPECT_EQ(rule.absence_began_ns(), now) << "an hour-old stamp of this process does not describe the venue";
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
