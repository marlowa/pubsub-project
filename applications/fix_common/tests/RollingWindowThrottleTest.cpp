// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "RollingWindowThrottle.hpp"
#include "ThrottleLimits.hpp"

namespace fix_common::tests {

namespace {

using Clock = RollingWindowThrottle::Clock;
using std::chrono::microseconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;

/// An arbitrary starting time, well clear of the clock's epoch.
const Clock::time_point start_time = Clock::time_point{} + std::chrono::hours(1000);

} // namespaces

TEST(RollingWindowThrottleTest, AcceptsExactlyTheLimitWithinASecondAndRefusesTheNext) {
    RollingWindowThrottle throttle(5);
    for (int command = 0; command < 5; ++command) {
        EXPECT_TRUE(throttle.try_accept(start_time + milliseconds(command * 100))) << "command " << command;
    }
    EXPECT_FALSE(throttle.try_accept(start_time + milliseconds(999)));
}

TEST(RollingWindowThrottleTest, AcceptsAllTheLimitAtTheSameInstant) {
    RollingWindowThrottle throttle(3);
    EXPECT_TRUE(throttle.try_accept(start_time));
    EXPECT_TRUE(throttle.try_accept(start_time));
    EXPECT_TRUE(throttle.try_accept(start_time));
    EXPECT_FALSE(throttle.try_accept(start_time));
}

TEST(RollingWindowThrottleTest, AcceptsOneMoreOnceASecondHasPassedSinceTheFirst) {
    RollingWindowThrottle throttle(3);
    ASSERT_TRUE(throttle.try_accept(start_time));
    ASSERT_TRUE(throttle.try_accept(start_time + milliseconds(300)));
    ASSERT_TRUE(throttle.try_accept(start_time + milliseconds(600)));
    ASSERT_FALSE(throttle.try_accept(start_time + milliseconds(900)));

    // The first has left the window, and only the first: one more, then refused again.
    EXPECT_TRUE(throttle.try_accept(start_time + milliseconds(1100)));
    EXPECT_FALSE(throttle.try_accept(start_time + milliseconds(1200)));
    // The second leaves at 1300 ms.
    EXPECT_TRUE(throttle.try_accept(start_time + milliseconds(1300)));
}

TEST(RollingWindowThrottleTest, ACommandExactlyOneSecondAfterTheOldestIsAccepted) {
    RollingWindowThrottle throttle(1);
    ASSERT_TRUE(throttle.try_accept(start_time));
    EXPECT_FALSE(throttle.try_accept(start_time + seconds(1) - nanoseconds(1)));
    EXPECT_TRUE(throttle.try_accept(start_time + seconds(1)));
}

TEST(RollingWindowThrottleTest, RefusedCommandsDoNotCount) {
    RollingWindowThrottle throttle(2);
    ASSERT_TRUE(throttle.try_accept(start_time));
    ASSERT_TRUE(throttle.try_accept(start_time + milliseconds(10)));
    // A member that keeps retrying while refused...
    for (int retry = 1; retry < 100; ++retry) {
        ASSERT_FALSE(throttle.try_accept(start_time + milliseconds(10 + retry * 9)));
    }
    EXPECT_EQ(throttle.recorded_count(), 2u);
    // ...is accepted again a second after its first accepted command, not a second after its last retry.
    EXPECT_TRUE(throttle.try_accept(start_time + seconds(1)));
}

TEST(RollingWindowThrottleTest, NoOneSecondPeriodEverHoldsMoreThanTheLimit) {
    // A steady stream at four times the limit, one command every 2.5 ms, for ten seconds. Every
    // accepted time is kept, and every window starting at an accepted time is counted.
    const int limit = 100;
    RollingWindowThrottle throttle(limit);
    std::vector<Clock::time_point> accepted;
    for (int command = 0; command < 4000; ++command) {
        const Clock::time_point now = start_time + microseconds(command * 2500);
        if (throttle.try_accept(now)) {
            accepted.push_back(now);
        }
    }
    size_t window_end = 0;
    for (size_t window_start = 0; window_start < accepted.size(); ++window_start) {
        while (window_end < accepted.size() && accepted[window_end] - accepted[window_start] < seconds(1)) {
            ++window_end;
        }
        ASSERT_LE(window_end - window_start, static_cast<size_t>(limit)) << "the period starting at accepted command " << window_start;
    }
    // And the throttle is not stricter than the rule: over ten seconds it accepted ten seconds' worth.
    EXPECT_EQ(accepted.size(), 1000u);
}

TEST(RollingWindowThrottleTest, StaysCorrectAfterWrappingManyTimes) {
    // At the limit, then a pause, over and over: the ring buffer's positions go round many times.
    RollingWindowThrottle throttle(7);
    Clock::time_point now = start_time;
    for (int burst = 0; burst < 500; ++burst) {
        for (int command = 0; command < 7; ++command) {
            ASSERT_TRUE(throttle.try_accept(now)) << "burst " << burst << ", command " << command;
            now += milliseconds(1);
        }
        ASSERT_FALSE(throttle.try_accept(now)) << "burst " << burst;
        now += seconds(1);
    }
}

TEST(RollingWindowThrottleTest, ALimitOutsideTheRangeIsRefused) {
    EXPECT_THROW(RollingWindowThrottle(0), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(RollingWindowThrottle(-1), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(RollingWindowThrottle(ThrottleLimits::max_permitted_per_second + 1), pubsub_itc_fw::PreconditionAssertion);
}

TEST(RollingWindowThrottleTest, TheLargestPermittedLimitIsAcceptedAndHoldsEightBytesForEachCommand) {
    const RollingWindowThrottle throttle(ThrottleLimits::max_permitted_per_second);
    EXPECT_EQ(throttle.max_per_second(), 100000);
    EXPECT_EQ(throttle.allocated_bytes(), 800000u);
}

TEST(RollingWindowThrottleTest, ItsStorageIsReportedToTheGrowthReporter) {
    pubsub_itc_fw::AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 0;
    int reports = 0;
    reporter.on_large_allocation = [&reports](size_t, size_t) { ++reports; };
    RollingWindowThrottle throttle(50, &reporter);
    EXPECT_EQ(reports, 1);
    int accepted = 0;
    for (int command = 0; command < 1000; ++command) {
        if (throttle.try_accept(start_time + milliseconds(command))) {
            ++accepted;
        }
    }
    EXPECT_EQ(accepted, 50);
    EXPECT_EQ(reports, 1);
}

} // namespaces
