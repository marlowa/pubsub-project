// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;
namespace pubsub_itc_fw::tests {

namespace {

constexpr auto interval = std::chrono::milliseconds(100);
constexpr auto long_interval = std::chrono::milliseconds(500);
constexpr int wait_milliseconds = 3000;

} // namespaces

// Fixture

class TimerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        ReactorConfiguration cfg;
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(50);
        cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);
        cfg.shutdown_timeout_ = std::chrono::milliseconds(200);
        reactor_ = std::make_unique<Reactor>(cfg, registry_, logger_->logger);
    }

    void TearDown() override {
        if (!reactor_->is_finished()) {
            reactor_->shutdown("test complete");
        }
        if (reactor_thread_.joinable()) {
            reactor_thread_.join();
        }
        reactor_.reset();
        logger_.reset();
    }

    void start_reactor() {
        reactor_thread_ = std::thread([this] { reactor_->run(); });
    }

    bool wait_for(std::function<bool()> pred, int timeout_ms = wait_milliseconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (reactor_->is_finished())
                return false;
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    std::unique_ptr<LoggerWithSink> logger_;
    ServiceRegistry registry_;
    std::unique_ptr<Reactor> reactor_;
    std::thread reactor_thread_;
};

// OneOffTimerThread -- starts a single one-off timer in on_app_ready_event

class OneOffTimerThread : public ApplicationThread {
  public:
    OneOffTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "OneOffTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("OneOffPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};
    mutable std::mutex ids_mutex;
    std::vector<TimerID> fired_ids;
    TimerID one_off_id{};

  protected:
    void on_app_ready_event() override {
        one_off_id = start_one_off_timer(interval);
    }

    void on_timer_event(TimerID id) override {
        std::lock_guard lock(ids_mutex);
        fired_ids.push_back(id);
        fire_count.fetch_add(1, std::memory_order_release);
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, OneOffTimerFiresExactlyOnce) {
    auto t = ApplicationThread::create<OneOffTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 1; })) << "One-off timer never fired";

    std::this_thread::sleep_for(interval * 3);
    EXPECT_EQ(t->fire_count.load(std::memory_order_acquire), 1) << "One-off timer fired more than once";
}

TEST_F(TimerTest, OneOffTimerDeliversCorrectIdToCallback) {
    auto t = ApplicationThread::create<OneOffTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 1; })) << "One-off timer never fired";

    reactor_->shutdown("test complete");
    reactor_thread_.join();

    std::lock_guard lock(t->ids_mutex);
    ASSERT_EQ(t->fired_ids.size(), 1u);
    EXPECT_EQ(t->fired_ids[0], t->one_off_id);
    EXPECT_TRUE(t->one_off_id.is_valid());
}

// RecurringTimerThread -- starts a recurring timer; self-cancels at cancel_after

class RecurringTimerThread : public ApplicationThread {
  public:
    RecurringTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "RecurringTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("RecurringPool"),
                            ApplicationThreadConfiguration{}) {}

    static constexpr int cancel_after = 4;
    std::atomic<int> fire_count{0};
    TimerID recurring_id{};

  protected:
    void on_app_ready_event() override {
        recurring_id = start_recurring_timer(interval);
    }

    void on_timer_event(TimerID id) override {
        if (id != recurring_id) {
            return;
        }
        const int n = fire_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n >= cancel_after) {
            cancel_timer(recurring_id);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, RecurringTimerFiresMultipleTimes) {
    auto t = ApplicationThread::create<RecurringTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= RecurringTimerThread::cancel_after; }))
        << "Recurring timer did not fire " << RecurringTimerThread::cancel_after << " times";
}

TEST_F(TimerTest, RecurringTimerStopsFiringAfterCancel) {
    auto t = ApplicationThread::create<RecurringTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= RecurringTimerThread::cancel_after; }))
        << "Recurring timer did not reach cancel threshold";

    // Allow time for the cancel command to be processed by the reactor.
    std::this_thread::sleep_for(interval * 2);
    const int stable_count = t->fire_count.load(std::memory_order_acquire);

    std::this_thread::sleep_for(interval * 3);
    EXPECT_EQ(t->fire_count.load(std::memory_order_acquire), stable_count) << "Recurring timer continued to fire after cancel";
}

// CancelBeforeExpiryThread -- starts a one-off and cancels it immediately

class CancelBeforeExpiryThread : public ApplicationThread {
  public:
    CancelBeforeExpiryThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "CancelBeforeExpiryThread", ThreadID{1}, make_queue_config(), make_allocator_config("CancelPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> ready{false};
    std::atomic<int> fire_count{0};

  protected:
    void on_app_ready_event() override {
        const TimerID id = start_one_off_timer(long_interval);
        cancel_timer(id);
        ready.store(true, std::memory_order_release);
    }

    void on_timer_event(TimerID) override {
        fire_count.fetch_add(1, std::memory_order_release);
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, CancelledOneOffTimerNeverFires) {
    auto t = ApplicationThread::create<CancelBeforeExpiryThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->ready.load(std::memory_order_acquire); })) << "Thread did not reach on_app_ready_event";

    std::this_thread::sleep_for(long_interval + interval);
    EXPECT_EQ(t->fire_count.load(std::memory_order_acquire), 0) << "Cancelled timer fired";
}

// CancelUnsetTimerThread -- cancels a default-constructed (never-armed) TimerID,
// then arms a real one. Cancelling id 0 must be a harmless no-op: id 0 is the
// invalid sentinel and must never collide with a real timer (e.g. the reactor
// backstop), so the reactor keeps running and the real timer still fires.

class CancelUnsetTimerThread : public ApplicationThread {
  public:
    CancelUnsetTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "CancelUnsetTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("CancelUnsetPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};
    TimerID real_id{};

  protected:
    void on_app_ready_event() override {
        cancel_timer(TimerID{}); // never-armed / unset -- must be a no-op, not a crash
        real_id = start_one_off_timer(interval);
    }

    void on_timer_event(TimerID id) override {
        if (id == real_id) {
            fire_count.fetch_add(1, std::memory_order_release);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, CancellingUnsetTimerIsHarmless) {
    auto t = ApplicationThread::create<CancelUnsetTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 1; }))
        << "Reactor did not survive cancelling an unset timer, or the real timer never fired";
    EXPECT_FALSE(reactor_->is_finished()) << "Reactor terminated after cancelling an unset timer";
}

// TwoTimersThread -- two independent one-off timers, distinguished by their ids

class TwoTimersThread : public ApplicationThread {
  public:
    TwoTimersThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TwoTimersThread", ThreadID{1}, make_queue_config(), make_allocator_config("TwoTimersPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> alpha_count{0};
    std::atomic<int> beta_count{0};
    TimerID alpha_id{};
    TimerID beta_id{};

  protected:
    void on_app_ready_event() override {
        alpha_id = start_one_off_timer(interval);
        beta_id = start_one_off_timer(interval * 2);
    }

    void on_timer_event(TimerID id) override {
        if (id == alpha_id) {
            alpha_count.fetch_add(1, std::memory_order_release);
        } else if (id == beta_id) {
            beta_count.fetch_add(1, std::memory_order_release);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, TwoIndependentTimersEachFireOnce) {
    auto t = ApplicationThread::create<TwoTimersThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->alpha_count.load(std::memory_order_acquire) >= 1; })) << "Timer 'alpha' never fired";
    ASSERT_TRUE(wait_for([&] { return t->beta_count.load(std::memory_order_acquire) >= 1; })) << "Timer 'beta' never fired";

    std::this_thread::sleep_for(interval * 3);
    EXPECT_EQ(t->alpha_count.load(std::memory_order_acquire), 1) << "'alpha' fired more than once";
    EXPECT_EQ(t->beta_count.load(std::memory_order_acquire), 1) << "'beta' fired more than once";
    EXPECT_NE(t->alpha_id, t->beta_id) << "each timer must get its own id";
}

// RescheduleTimerThread -- reschedule a one-off by starting a fresh one from the callback

class RescheduleTimerThread : public ApplicationThread {
  public:
    RescheduleTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "RescheduleTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("ReschedulePool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};
    TimerID current_id{};

  protected:
    void on_app_ready_event() override {
        current_id = start_one_off_timer(interval);
    }

    void on_timer_event(TimerID id) override {
        if (id != current_id) {
            return;
        }
        const int n = fire_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n == 1) {
            // On the first fire, reschedule: a fresh one-off with a new id.
            current_id = start_one_off_timer(interval);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, RescheduledTimerFiresTwice) {
    auto t = ApplicationThread::create<RescheduleTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 2; })) << "Timer did not fire twice after rescheduling";
}

// ManyTimersThread -- several independent one-off timers, each identified by its id.
// Exercises the reactor holding multiple timerfds for one thread simultaneously.

class ManyTimersThread : public ApplicationThread {
  public:
    static constexpr int timer_count = 5;

    ManyTimersThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "ManyTimersThread", ThreadID{1}, make_queue_config(), make_allocator_config("ManyTimersPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> distinct_fired{0};

  protected:
    void on_app_ready_event() override {
        for (int index = 0; index < timer_count; ++index) {
            ids_[index] = start_one_off_timer(interval + std::chrono::milliseconds(10 * index));
        }
    }

    void on_timer_event(TimerID id) override {
        for (int index = 0; index < timer_count; ++index) {
            if (id == ids_[index] && !seen_[index]) {
                seen_[index] = true;
                distinct_fired.fetch_add(1, std::memory_order_release);
                return;
            }
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    TimerID ids_[timer_count]{};
    bool seen_[timer_count]{};
};

TEST_F(TimerTest, ManyIndependentTimersAllFireOnce) {
    auto t = ApplicationThread::create<ManyTimersThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->distinct_fired.load(std::memory_order_acquire) >= ManyTimersThread::timer_count; }))
        << "Not all independent timers fired";
}

// CancelOneOfTwoThread -- two recurring timers; cancel one, the other keeps firing.
// Exercises cancel_timer_fd removing a single timerfd while leaving the thread's
// other timer registered.

class CancelOneOfTwoThread : public ApplicationThread {
  public:
    CancelOneOfTwoThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "CancelOneOfTwoThread", ThreadID{1}, make_queue_config(), make_allocator_config("CancelOneOfTwoPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> keep_count{0};
    std::atomic<int> drop_count{0};
    std::atomic<bool> dropped{false};
    TimerID keep_id{};
    TimerID drop_id{};

  protected:
    void on_app_ready_event() override {
        keep_id = start_recurring_timer(interval);
        drop_id = start_recurring_timer(interval);
    }

    void on_timer_event(TimerID id) override {
        if (id == keep_id) {
            const int n = keep_count.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (n == 2 && !dropped.load(std::memory_order_acquire)) {
                cancel_timer(drop_id);
                dropped.store(true, std::memory_order_release);
            }
        } else if (id == drop_id) {
            drop_count.fetch_add(1, std::memory_order_release);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, CancellingOneTimerLeavesTheOtherRunning) {
    auto t = ApplicationThread::create<CancelOneOfTwoThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->dropped.load(std::memory_order_acquire); })) << "Never reached the cancel point";

    std::this_thread::sleep_for(interval * 2);
    const int drop_at_cancel = t->drop_count.load(std::memory_order_acquire);
    const int keep_at_cancel = t->keep_count.load(std::memory_order_acquire);

    // The kept timer must keep firing; the dropped one must stop.
    ASSERT_TRUE(wait_for([&] { return t->keep_count.load(std::memory_order_acquire) > keep_at_cancel; })) << "Kept timer stopped firing";
    std::this_thread::sleep_for(interval * 2);
    EXPECT_EQ(t->drop_count.load(std::memory_order_acquire), drop_at_cancel) << "Cancelled timer kept firing";
}

// SlowHandlerThread -- a recurring timer whose handler is slower than the interval,
// so the reactor reads multiple expirations at once. Exercises the coalescing
// branch in TimerHandler (expirations > 1 delivered as a single event).

class SlowHandlerThread : public ApplicationThread {
  public:
    SlowHandlerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "SlowHandlerThread", ThreadID{1}, make_queue_config(), make_allocator_config("SlowHandlerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};
    std::atomic<bool> done{false};
    TimerID recurring_id{};

  protected:
    void on_app_ready_event() override {
        recurring_id = start_recurring_timer(std::chrono::milliseconds(20));
    }

    void on_timer_event(TimerID id) override {
        if (id != recurring_id || done.load(std::memory_order_acquire)) {
            return; // once cancelled, drain any queued backlog without sleeping
        }
        const int n = fire_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Block long enough that several timerfd expirations accumulate before the
        // next read, so the reactor coalesces them into one delivered event.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        if (n >= 3) {
            cancel_timer(recurring_id);
            done.store(true, std::memory_order_release);
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

TEST_F(TimerTest, SlowHandlerCoalescesExpirations) {
    auto t = ApplicationThread::create<SlowHandlerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 3; })) << "Slow-handler recurring timer did not fire enough times";
}

} // namespaces
