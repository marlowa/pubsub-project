// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/HighResolutionClock.hpp>

using namespace pubsub_itc_fw;

TEST(HighResolutionClockTest, NowDoesNotGoBackwards) {
    auto t1 = HighResolutionClock::now();
    auto t2 = HighResolutionClock::now();

    EXPECT_LE(t1, t2);
}

TEST(HighResolutionClockTest, MonotonicAcrossShortSleep) {
    auto t1 = HighResolutionClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto t2 = HighResolutionClock::now();

    EXPECT_LT(t1, t2);
}

TEST(HighResolutionClockTest, NanosecondResolutionIsReasonable) {
    auto t1 = HighResolutionClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto t2 = HighResolutionClock::now();

    const auto elapsed_ns = (t2 - t1).count();
    EXPECT_GE(elapsed_ns, 1'000'000LL) << "Expected at least 1ms elapsed in nanoseconds";
}

TEST(HighResolutionClockTest, ManySequentialCallsAreMonotonic) {
    auto prev = HighResolutionClock::now();

    for (int i = 0; i < 10000; ++i) {
        auto now = HighResolutionClock::now();
        EXPECT_LE(prev, now);
        prev = now;
    }
}

TEST(HighResolutionClockTest, DurationTypeIsNanoseconds) {
    static_assert(std::is_same_v<HighResolutionClock::duration, std::chrono::nanoseconds>, "HighResolutionClock::duration must be nanoseconds");
}
