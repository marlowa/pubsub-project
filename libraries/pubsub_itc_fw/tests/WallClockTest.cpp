// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/WallClock.hpp>

using namespace pubsub_itc_fw;

namespace {

int64_t system_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespaces

TEST(WallClockTest, SystemWallClockReturnsCurrentTime) {
    SystemWallClock clock;

    const int64_t before = system_now_ns();
    const int64_t sampled = clock.now_ns();
    const int64_t after = system_now_ns();

    EXPECT_GE(sampled, before);
    EXPECT_LE(sampled, after);
}

TEST(WallClockTest, SystemWallClockIsNonDecreasing) {
    SystemWallClock clock;

    const int64_t first = clock.now_ns();
    const int64_t second = clock.now_ns();

    EXPECT_GE(second, first);
}

TEST(WallClockTest, ReplayClockDefaultsToZero) {
    ReplayClock clock;

    EXPECT_EQ(clock.now_ns(), 0);
}

TEST(WallClockTest, ReplayClockReturnsInitialValue) {
    const int64_t initial = 1234567890123456789;
    ReplayClock clock(initial);

    EXPECT_EQ(clock.now_ns(), initial);
}

TEST(WallClockTest, ReplayClockSetTimeAdvancesTheClock) {
    ReplayClock clock;

    clock.set_time_ns(42);
    EXPECT_EQ(clock.now_ns(), 42);

    clock.set_time_ns(1000);
    EXPECT_EQ(clock.now_ns(), 1000);
}

TEST(WallClockTest, ReplayClockIsUsableThroughTheBaseInterface) {
    ReplayClock concrete(500);
    const WallClock& base = concrete;

    EXPECT_EQ(base.now_ns(), 500);
}
