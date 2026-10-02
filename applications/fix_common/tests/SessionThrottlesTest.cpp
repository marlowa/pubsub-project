// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <utility>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "SessionThrottles.hpp"
#include "ThrottleLimits.hpp"
#include "ThrottleOutcome.hpp"
#include "ThrottledCommand.hpp"

namespace fix_common::tests {

namespace {

using Clock = SessionThrottles::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

const Clock::time_point start_time = Clock::time_point{} + std::chrono::hours(1000);

ThrottleLimits make_limits(int place, int amend, int cancel) {
    ThrottleLimits limits;
    limits.max_place_per_second = place;
    limits.max_amend_per_second = amend;
    limits.max_cancel_per_second = cancel;
    return limits;
}

} // namespaces

TEST(SessionThrottlesTest, ALimitOfZeroCreatesNoThrottleAndAcceptsEverything) {
    SessionThrottles throttles(make_limits(0, 0, 0));
    EXPECT_EQ(throttles.allocated_bytes(), 0u);
    for (int command = 0; command < 10000; ++command) {
        ASSERT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time), ThrottleOutcome::Accepted);
        ASSERT_EQ(throttles.try_accept(ThrottledCommand::Amend, start_time), ThrottleOutcome::Accepted);
        ASSERT_EQ(throttles.try_accept(ThrottledCommand::Cancel, start_time), ThrottleOutcome::Accepted);
    }
    EXPECT_EQ(throttles.max_per_second(ThrottledCommand::Place), 0);
    EXPECT_TRUE(throttles.refusal_text(ThrottledCommand::Place).empty());
}

TEST(SessionThrottlesTest, ADefaultConstructedSetAcceptsEverything) {
    SessionThrottles throttles;
    for (int command = 0; command < 1000; ++command) {
        ASSERT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Cancel, start_time)));
    }
    EXPECT_EQ(throttles.allocated_bytes(), 0u);
}

TEST(SessionThrottlesTest, TheThreeKindsAreThrottledIndependently) {
    SessionThrottles throttles(make_limits(2, 1, 3));
    EXPECT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));
    EXPECT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));
    EXPECT_FALSE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));

    // Place being exhausted takes nothing from the others.
    EXPECT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Amend, start_time)));
    EXPECT_FALSE(is_accepted(throttles.try_accept(ThrottledCommand::Amend, start_time)));
    for (int command = 0; command < 3; ++command) {
        EXPECT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Cancel, start_time)));
    }
    EXPECT_FALSE(is_accepted(throttles.try_accept(ThrottledCommand::Cancel, start_time)));
}

TEST(SessionThrottlesTest, OneKindWithNoLimitBesideTwoThatHaveOne) {
    SessionThrottles throttles(make_limits(1, 0, 1));
    EXPECT_EQ(throttles.allocated_bytes(), 2 * sizeof(Clock::time_point));
    for (int command = 0; command < 100; ++command) {
        ASSERT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Amend, start_time)));
    }
    EXPECT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));
    EXPECT_FALSE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));
}

TEST(SessionThrottlesTest, TwoSessionsAreThrottledIndependently) {
    const ThrottleLimits limits = make_limits(1, 1, 1);
    SessionThrottles first_session(limits);
    SessionThrottles second_session(limits);
    EXPECT_TRUE(is_accepted(first_session.try_accept(ThrottledCommand::Place, start_time)));
    EXPECT_FALSE(is_accepted(first_session.try_accept(ThrottledCommand::Place, start_time)));
    EXPECT_TRUE(is_accepted(second_session.try_accept(ThrottledCommand::Place, start_time)));
}

TEST(SessionThrottlesTest, TheOutcomeMarksWhereARunOfRefusalsBeginsAndEnds) {
    SessionThrottles throttles(make_limits(1, 0, 0));
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time), ThrottleOutcome::Accepted);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + milliseconds(1)), ThrottleOutcome::FirstRefusal);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + milliseconds(2)), ThrottleOutcome::FurtherRefusal);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + milliseconds(3)), ThrottleOutcome::FurtherRefusal);
    EXPECT_EQ(throttles.refusals_in_current_run(ThrottledCommand::Place), 3);
    EXPECT_EQ(throttles.refusals_in_last_run(ThrottledCommand::Place), 0);

    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + seconds(1)), ThrottleOutcome::AcceptedAfterRefusals);
    EXPECT_EQ(throttles.refusals_in_current_run(ThrottledCommand::Place), 0);
    EXPECT_EQ(throttles.refusals_in_last_run(ThrottledCommand::Place), 3);

    // The next run begins afresh.
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + seconds(1)), ThrottleOutcome::FirstRefusal);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + seconds(2)), ThrottleOutcome::AcceptedAfterRefusals);
    EXPECT_EQ(throttles.refusals_in_last_run(ThrottledCommand::Place), 1);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time + seconds(3)), ThrottleOutcome::Accepted);
}

TEST(SessionThrottlesTest, RunsOfRefusalsAreTrackedForEachKindSeparately) {
    SessionThrottles throttles(make_limits(1, 1, 1));
    ASSERT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Place, start_time)));
    ASSERT_TRUE(is_accepted(throttles.try_accept(ThrottledCommand::Cancel, start_time)));
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time), ThrottleOutcome::FirstRefusal);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Cancel, start_time), ThrottleOutcome::FirstRefusal);
    EXPECT_EQ(throttles.try_accept(ThrottledCommand::Place, start_time), ThrottleOutcome::FurtherRefusal);
    EXPECT_EQ(throttles.refusals_in_current_run(ThrottledCommand::Place), 2);
    EXPECT_EQ(throttles.refusals_in_current_run(ThrottledCommand::Cancel), 1);
    EXPECT_EQ(throttles.refusals_in_current_run(ThrottledCommand::Amend), 0);
}

TEST(SessionThrottlesTest, TheRefusalTextStatesTheLimitTheKindAndThatItIsForEachSession) {
    const SessionThrottles throttles(make_limits(50, 10, 200));
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Place), "Throttled: at most 50 new orders per second for this session");
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Amend), "Throttled: at most 10 amends per second for this session");
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Cancel), "Throttled: at most 200 cancels per second for this session");
}

TEST(SessionThrottlesTest, TheRefusalTextUsesTheSingularForALimitOfOne) {
    const SessionThrottles throttles(make_limits(1, 1, 1));
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Place), "Throttled: at most 1 new order per second for this session");
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Amend), "Throttled: at most 1 amend per second for this session");
    EXPECT_EQ(throttles.refusal_text(ThrottledCommand::Cancel), "Throttled: at most 1 cancel per second for this session");
}

TEST(SessionThrottlesTest, TheLimitAppliedIsTheLimitGiven) {
    const SessionThrottles throttles(make_limits(7, 0, 100000));
    EXPECT_EQ(throttles.max_per_second(ThrottledCommand::Place), 7);
    EXPECT_EQ(throttles.max_per_second(ThrottledCommand::Amend), 0);
    EXPECT_EQ(throttles.max_per_second(ThrottledCommand::Cancel), 100000);
    EXPECT_EQ(throttles.allocated_bytes(), (7u + 100000u) * sizeof(Clock::time_point));
}

TEST(SessionThrottlesTest, ALimitOutsideTheRangeIsRefused) {
    EXPECT_THROW(SessionThrottles(make_limits(-1, 0, 0)), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(SessionThrottles(make_limits(0, 0, ThrottleLimits::max_permitted_per_second + 1)), pubsub_itc_fw::PreconditionAssertion);
}

TEST(SessionThrottlesTest, MovingASetKeepsItsCountsAndItsText) {
    SessionThrottles original(make_limits(1, 0, 0));
    ASSERT_TRUE(is_accepted(original.try_accept(ThrottledCommand::Place, start_time)));
    SessionThrottles moved(std::move(original));
    EXPECT_EQ(moved.try_accept(ThrottledCommand::Place, start_time), ThrottleOutcome::FirstRefusal);
    EXPECT_EQ(moved.refusal_text(ThrottledCommand::Place), "Throttled: at most 1 new order per second for this session");
}

} // namespaces
