#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MillisecondClock.hpp>

using namespace pubsub_itc_fw;

TEST(MillisecondClockTest, NowDoesNotGoBackwards)
{
    auto t1 = MillisecondClock::now();
    auto t2 = MillisecondClock::now();

    EXPECT_LE(t1.time_since_epoch().count(), t2.time_since_epoch().count());
}

TEST(MillisecondClockTest, MonotonicAcrossShortSleep)
{
    auto t1 = MillisecondClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto t2 = MillisecondClock::now();

    EXPECT_LT(t1, t2);
}

TEST(MillisecondClockTest, MillisecondResolutionIsReasonable)
{
    auto t1 = MillisecondClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto t2 = MillisecondClock::now();

    // We cannot assert exact resolution, but we can assert non-zero progress.
    EXPECT_LE(1, (t2 - t1).count());
}

TEST(MillisecondClockTest, ManySequentialCallsAreMonotonic)
{
    auto prev = MillisecondClock::now();

    for (int i = 0; i < 10000; ++i) {
        auto now = MillisecondClock::now();
        EXPECT_LE(prev, now);
        prev = now;
    }
}
