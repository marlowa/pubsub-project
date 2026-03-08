/**
 * @brief Unit tests for the ThreadWithJoinTimeout class.
 *
 * This file contains a suite of Google Tests to verify the correctness and
 * robustness of the `ThreadWithJoinTimeout` thread wrapper.
 */

#include <chrono>
#include <thread>
#include <future>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace pubsub_itc_fw {

// Define a test fixture for ThreadWithJoinTimeout
class ThreadWithJoinTimeoutTest : public ::testing::Test {};

// Test that a thread which finishes quickly is joined successfully
TEST_F(ThreadWithJoinTimeoutTest, JoinCompletesWithinTimeout) {
    ThreadWithJoinTimeout t;

    t.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    const bool joined = t.join_with_timeout(std::chrono::milliseconds(200));
    EXPECT_TRUE(joined);
    EXPECT_FALSE(t.joinable());
}

// Test that a thread which does not finish in time triggers a timeout
TEST_F(ThreadWithJoinTimeoutTest, JoinTimesOut) {
    ThreadWithJoinTimeout t;

    t.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    const bool joined = t.join_with_timeout(std::chrono::milliseconds(100));
    EXPECT_FALSE(joined);
    EXPECT_FALSE(t.joinable());

    // Give the worker time to finish so its functor is freed
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

#ifndef USING_VALGRIND
TEST_F(ThreadWithJoinTimeoutTest, JoinTimesOutOnTrulyStuckThread) {
    ThreadWithJoinTimeout t;

    t.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // sleep long past the join timeout
    });

    const bool joined = t.join_with_timeout(std::chrono::milliseconds(100));
    EXPECT_FALSE(joined);
    EXPECT_FALSE(t.joinable());

    // Destructor must be safe after timeout
    SUCCEED() << "Stuck thread timed out and wrapper cleaned up safely.";
}
#endif

// Test that destructor is safe after a timeout
TEST_F(ThreadWithJoinTimeoutTest, DestructorSafeAfterTimeout) {
    {
        ThreadWithJoinTimeout t;

        t.start([] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        });

        const bool joined = t.join_with_timeout(std::chrono::milliseconds(100));
        EXPECT_FALSE(joined);
        EXPECT_FALSE(t.joinable());
    }

    SUCCEED() << "Destructor executed safely after timeout.";
}

// Test that join() works normally
TEST_F(ThreadWithJoinTimeoutTest, NormalJoin) {
    ThreadWithJoinTimeout t;

    t.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    EXPECT_TRUE(t.joinable());
    t.join();
    EXPECT_FALSE(t.joinable());
}

// Test that calling start() twice without joining throws
TEST_F(ThreadWithJoinTimeoutTest, StartTwiceThrows) {
    ThreadWithJoinTimeout t;

    t.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    EXPECT_THROW(
        t.start([] {}),
        std::runtime_error
    );

    t.join();
}

// Test move construction
TEST_F(ThreadWithJoinTimeoutTest, MoveConstructorTransfersOwnership) {
    ThreadWithJoinTimeout t1;

    t1.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    EXPECT_TRUE(t1.joinable());

    ThreadWithJoinTimeout t2(std::move(t1));

    EXPECT_FALSE(t1.joinable());
    EXPECT_TRUE(t2.joinable());

    t2.join();
}

// Test move assignment
TEST_F(ThreadWithJoinTimeoutTest, MoveAssignmentTransfersOwnership) {
    ThreadWithJoinTimeout t1;
    ThreadWithJoinTimeout t2;

    t1.start([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    EXPECT_TRUE(t1.joinable());
    EXPECT_FALSE(t2.joinable());

    t2 = std::move(t1);

    EXPECT_FALSE(t1.joinable());
    EXPECT_TRUE(t2.joinable());

    t2.join();
}

} // namespace pubsub_itc_fw
