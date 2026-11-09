// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MessagesWaitingLimit.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw::tests {

TEST(MessagesWaitingLimitTest, ReadingGoesOnBelowTheLimitAndStopsAtIt) {
    MessagesWaitingLimit limit(16);
    for (int index = 0; index < 15; ++index) {
        ASSERT_FALSE(limit.message_queued()) << "message " << index + 1;
    }
    EXPECT_FALSE(limit.stopped());
    EXPECT_TRUE(limit.message_queued()) << "the sixteenth";
    EXPECT_TRUE(limit.stopped());
    EXPECT_EQ(limit.waiting(), 16);
}

TEST(MessagesWaitingLimitTest, ReadingStartsAgainOnceTheWaitingAreTakenDownToHalfTheLimit) {
    MessagesWaitingLimit limit(16);
    for (int index = 0; index < 16; ++index) {
        static_cast<void>(limit.message_queued());
    }
    for (int index = 0; index < 7; ++index) {
        ASSERT_FALSE(limit.message_taken()) << "still " << limit.waiting() << " waiting";
    }
    EXPECT_TRUE(limit.message_taken()) << "down to 8, half of 16";
    EXPECT_FALSE(limit.stopped());
}

TEST(MessagesWaitingLimitTest, ReadingIsAskedForAgainOnlyOnce) {
    MessagesWaitingLimit limit(16);
    for (int index = 0; index < 16; ++index) {
        static_cast<void>(limit.message_queued());
    }
    int asked = 0;
    for (int index = 0; index < 16; ++index) {
        asked += limit.message_taken() ? 1 : 0;
    }
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(limit.waiting(), 0);
}

TEST(MessagesWaitingLimitTest, TakingMessagesWhenReadingWasNeverStoppedNeverAsksForIt) {
    MessagesWaitingLimit limit(16);
    for (int round = 0; round < 100; ++round) {
        for (int index = 0; index < 10; ++index) {
            ASSERT_FALSE(limit.message_queued());
        }
        for (int index = 0; index < 10; ++index) {
            ASSERT_FALSE(limit.message_taken());
        }
    }
}

TEST(MessagesWaitingLimitTest, IfEveryMessageIsTakenBeforeTheStopIsRecordedTheReactorDoesNotStop) {
    // The thread empties its queue at the moment between the reactor reaching the limit and recording the stop.
    MessagesWaitingLimit limit(16);
    bool application_asked_to_read_again = false;
    limit.set_between_reaching_the_limit_and_recording_the_stop([&] {
        while (limit.waiting() > 0) {
            application_asked_to_read_again = limit.message_taken() || application_asked_to_read_again;
        }
    });
    for (int index = 0; index < 15; ++index) {
        ASSERT_FALSE(limit.message_queued());
    }
    const bool stop = limit.message_queued();
    EXPECT_FALSE(application_asked_to_read_again) << "nothing was recorded as stopped when the thread took the messages";
    EXPECT_FALSE(stop) << "the reactor stopped with nothing waiting and nobody to start it again";
    EXPECT_FALSE(limit.stopped());
}

TEST(MessagesWaitingLimitTest, AThreadThatFoundTheCountLowDoesNotEndAStopMadeSinceWithTheLimitWaiting) {
    // The thread takes a message and finds the count low; before it looks at the stop, the reactor fills the queue to the
    // limit and stops.
    MessagesWaitingLimit limit(16);
    static_cast<void>(limit.message_queued());
    bool reactor_stopped = false;
    limit.set_between_taking_and_looking_at_the_stop([&] {
        while (!reactor_stopped) {
            reactor_stopped = limit.message_queued();
        }
    });
    const bool asked = limit.message_taken();
    EXPECT_EQ(limit.waiting(), 16);
    EXPECT_FALSE(asked) << "reading would start again with the limit's worth waiting";
    EXPECT_TRUE(limit.stopped());
}

TEST(MessagesWaitingLimitTest, ALimitBelowTheSmallestIsRefused) {
    EXPECT_THROW(MessagesWaitingLimit(15), PreconditionAssertion);
}

// The two threads as the reactor and an application thread use them, as fast as they can, with the smallest limit, so
// that the moment between the reactor reaching the limit and recording the stop is crossed as often as possible. The
// reactor stops whenever told to and waits to be told to read again; if a stop were ever left with nobody to end it, it
// would wait for good and the test would not finish in time.
TEST(MessagesWaitingLimitTest, TwoThreadsNeverLeaveReadingStoppedWithNothingWaiting) {
    constexpr int64_t messages = 2'000'000;
    MessagesWaitingLimit limit(MessagesWaitingLimit::smallest_limit);
    std::atomic<bool> read_again{false};
    std::atomic<int64_t> queued{0};
    std::atomic<int64_t> most_waiting{0};
    std::atomic<bool> reactor_stuck{false};

    std::thread reactor([&] {
        for (int64_t index = 0; index < messages; ++index) {
            // Counted before it is made available to the application thread, as the reactor counts a message before it
            // puts it on the thread's queue: counted after, the thread could take it first and leave the count one short.
            const bool stop = limit.message_queued();
            queued.fetch_add(1, std::memory_order_seq_cst);
            int64_t seen = limit.waiting();
            int64_t most = most_waiting.load();
            while (seen > most && !most_waiting.compare_exchange_weak(most, seen)) {}
            if (stop) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                while (!read_again.exchange(false, std::memory_order_seq_cst)) {
                    if (std::chrono::steady_clock::now() > deadline) {
                        reactor_stuck.store(true);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        }
    });

    std::thread application([&] {
        int64_t taken = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
        while (taken < messages && !reactor_stuck.load() && std::chrono::steady_clock::now() < deadline) {
            if (queued.load(std::memory_order_seq_cst) == taken) {
                std::this_thread::yield();
                continue;
            }
            ++taken;
            if (limit.message_taken()) {
                read_again.store(true, std::memory_order_seq_cst);
            }
        }
    });

    reactor.join();
    application.join();
    EXPECT_FALSE(reactor_stuck.load()) << "reading was left stopped with " << limit.waiting() << " waiting";
    EXPECT_EQ(queued.load(), messages);
    EXPECT_EQ(limit.waiting(), 0);
    EXPECT_LE(most_waiting.load(), MessagesWaitingLimit::smallest_limit);
}

} // namespaces
