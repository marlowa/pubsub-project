// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

namespace pubsub_itc_fw::tests {

namespace {

constexpr auto interval = std::chrono::milliseconds(100);
constexpr auto long_interval = std::chrono::milliseconds(500);
constexpr int wait_milliseconds = 3000;

} // namespaces

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// OneOffTimerThread -- starts a single one-off timer in on_app_ready_event
// ---------------------------------------------------------------------------

class OneOffTimerThread : public ApplicationThread {
  public:
    OneOffTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "OneOffTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("OneOffPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};
    mutable std::mutex names_mutex;
    std::vector<std::string> fired_names;

  protected:
    void on_app_ready_event() override {
        start_one_off_timer("one-off", interval);
    }

    void on_timer_event(const std::string& name) override {
        std::lock_guard lock(names_mutex);
        fired_names.push_back(name);
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

TEST_F(TimerTest, OneOffTimerDeliversCorrectNameToCallback) {
    auto t = ApplicationThread::create<OneOffTimerThread>(logger_->logger, *reactor_);
    reactor_->register_thread(t);
    start_reactor();

    ASSERT_TRUE(wait_for([&] { return t->fire_count.load(std::memory_order_acquire) >= 1; })) << "One-off timer never fired";

    reactor_->shutdown("test complete");
    reactor_thread_.join();

    std::lock_guard lock(t->names_mutex);
    ASSERT_EQ(t->fired_names.size(), 1u);
    EXPECT_EQ(t->fired_names[0], "one-off");
}

// ---------------------------------------------------------------------------
// RecurringTimerThread -- starts a recurring timer; self-cancels at cancel_after
// ---------------------------------------------------------------------------

class RecurringTimerThread : public ApplicationThread {
  public:
    RecurringTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "RecurringTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("RecurringPool"),
                            ApplicationThreadConfiguration{}) {}

    static constexpr int cancel_after = 4;
    std::atomic<int> fire_count{0};

  protected:
    void on_app_ready_event() override {
        start_recurring_timer("heartbeat", interval);
    }

    void on_timer_event(const std::string& name) override {
        const int n = fire_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n >= cancel_after) {
            cancel_timer(name);
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

// ---------------------------------------------------------------------------
// CancelBeforeExpiryThread -- starts a one-off and cancels it immediately
// ---------------------------------------------------------------------------

class CancelBeforeExpiryThread : public ApplicationThread {
  public:
    CancelBeforeExpiryThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "CancelBeforeExpiryThread", ThreadID{1}, make_queue_config(), make_allocator_config("CancelPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> ready{false};
    std::atomic<int> fire_count{0};

  protected:
    void on_app_ready_event() override {
        start_one_off_timer("cancel-me", long_interval);
        cancel_timer("cancel-me");
        ready.store(true, std::memory_order_release);
    }

    void on_timer_event(const std::string&) override {
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

// ---------------------------------------------------------------------------
// TwoTimersThread -- two independent one-off timers with different names
// ---------------------------------------------------------------------------

class TwoTimersThread : public ApplicationThread {
  public:
    TwoTimersThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TwoTimersThread", ThreadID{1}, make_queue_config(), make_allocator_config("TwoTimersPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> alpha_count{0};
    std::atomic<int> beta_count{0};

  protected:
    void on_app_ready_event() override {
        start_one_off_timer("alpha", interval);
        start_one_off_timer("beta", interval * 2);
    }

    void on_timer_event(const std::string& name) override {
        if (name == "alpha") {
            alpha_count.fetch_add(1, std::memory_order_release);
        } else if (name == "beta") {
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
}

// ---------------------------------------------------------------------------
// RescheduleTimerThread -- rescheduling a timer by reusing its name
// ---------------------------------------------------------------------------

class RescheduleTimerThread : public ApplicationThread {
  public:
    RescheduleTimerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "RescheduleTimerThread", ThreadID{1}, make_queue_config(), make_allocator_config("ReschedulePool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> fire_count{0};

  protected:
    void on_app_ready_event() override {
        start_one_off_timer("rescheduled", interval);
    }

    void on_timer_event(const std::string& name) override {
        const int n = fire_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n == 1) {
            // On the first fire reschedule the same-named timer.
            start_one_off_timer(name, interval);
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

} // namespaces
