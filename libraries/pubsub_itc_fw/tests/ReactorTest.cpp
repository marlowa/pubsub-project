// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <sys/epoll.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Reactor.hpp>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BackoffWithYield.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorLifecycleState.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/MisbehavingThreads.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::NeverStartingThread;
using pubsub_itc_fw::tests_common::RogueITCThread;
using pubsub_itc_fw::tests_common::ThrowingAppReadyThread;
using pubsub_itc_fw::tests_common::ThrowingDuringRunThread;
using pubsub_itc_fw::tests_common::ThrowingInitialThread;
using pubsub_itc_fw::tests_common::ThrowingTerminationThread;
// Reactor initialization timeout test

using namespace pubsub_itc_fw;
using namespace pubsub_itc_fw::tests_common;

namespace {

// Helpers: QueueConfiguration, AllocatorConfiguration
// Note: the parameter values here are different from the helpers in TestConfigurations
pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 3;
    cfg.for_client_use = nullptr;
    cfg.gone_below_low_watermark_handler = nullptr;
    cfg.gone_above_high_watermark_handler = nullptr;
    return cfg;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config() {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name = "ATestPool";
    cfg.objects_per_pool = 128;
    cfg.initial_pools = 1;
    cfg.expansion_threshold_hint = 0;
    cfg.handler_for_pool_exhausted = nullptr;
    cfg.handler_for_invalid_free = nullptr;
    cfg.handler_for_huge_pages_error = nullptr;
    cfg.use_huge_pages_flag = pubsub_itc_fw::UseHugePagesFlag(pubsub_itc_fw::UseHugePagesFlag::DoNotUseHugePages);
    cfg.context = nullptr;
    return cfg;
}

class ReactorTestEnv : public ::testing::Environment {
  public:
    LoggerWithSink* logger_with_sink = nullptr;
    Reactor* reactor = nullptr;

    void SetUp() override {
        // Create long-lived logger + sink
        logger_with_sink = new LoggerWithSink();
    }

    void TearDown() override {
        delete logger_with_sink;
    }
};

// Register this environment ONLY for this translation unit
static ::testing::Environment* const reactor_env = ::testing::AddGlobalTestEnvironment(new ReactorTestEnv());

static ReactorTestEnv* env() {
    return static_cast<ReactorTestEnv*>(reactor_env);
}

class ReactorTest : public ::testing::Test {
  public:
    ReactorTest() : logger_with_sink_(*env()->logger_with_sink) {}

    void SetUp() override {
        logger_with_sink_.clear();
        reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds(100);
#ifdef USING_VALGRIND
        // TSan (and Valgrind) add significant instrumentation overhead which
        // can delay thread startup. Use a longer timeout to avoid intermittent
        // failures caused by scheduling delays under instrumentation.
        reactor_configuration_.init_phase_timeout_ = MillisecondClock::duration{10000};
#else
        reactor_configuration_.init_phase_timeout_ = MillisecondClock::duration{2000};
#endif
        reactor_configuration_.shutdown_timeout_ = std::chrono::milliseconds(50);
        reactor_ = std::make_unique<Reactor>(reactor_configuration_, service_registry_, logger_with_sink_.logger);
        reactor_thread_.reset(); // not started yet
    }

    void TearDown() override {
        if (!reactor_->is_finished()) {
            reactor_->shutdown("Test End, forcing reactor shutdown");
        }
        if (reactor_thread_) {
            join_reactor_or_die(std::chrono::seconds(2));
        }
        reactor_.reset();
    }

    void join_reactor_or_die(std::chrono::milliseconds timeout) const {
        ASSERT_TRUE(reactor_thread_); // if this fails, it's a test bug

        if (!reactor_thread_->join_with_timeout(timeout)) {
            std::cerr << "FATAL: reactor thread did not join (timeout " << timeout.count() << " ms)\n";
            std::terminate();
        }
    }

    LoggerWithSink& logger_with_sink_;
    ReactorConfiguration reactor_configuration_;
    ServiceRegistry service_registry_;
    std::unique_ptr<Reactor> reactor_;
    std::unique_ptr<ThreadWithJoinTimeout> reactor_thread_;
};

// A cooperative thread that does nothing special at all
class CooperativeShutdownThread : public ApplicationThread {
  public:
    CooperativeShutdownThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& name, ThreadID id, const QueueConfiguration& qc,
                              const AllocatorConfiguration& ac)
        : ApplicationThread(token, logger, reactor, name, id, qc, ac, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {
        // Nothing special for this test.
    }

    void on_app_ready_event() override {
        // Nothing special for this test.
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {
        // Nothing special for this test.
    }
};

// A test ApplicationThread that records Init and AppReady events.
class TestApplicationThread : public ApplicationThread {
  public:
    TestApplicationThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& name, ThreadID id,
                          const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, name, id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

    std::atomic<bool> saw_initial_event{false};
    std::atomic<bool> saw_app_ready_event{false};

  protected:
    void on_initial_event() override {
        saw_initial_event.store(true, std::memory_order_release);
    }

    void on_app_ready_event() override {
        // Enforce local ordering: AppReady must not arrive before Init.
        EXPECT_TRUE(saw_initial_event.load(std::memory_order_acquire));
        saw_app_ready_event.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& event_message) override {
        // Not used in this test.
    }
};

/**
 * @brief Schedules a number of recurring timers and records which of them fire.
 *
 * Scheduling a timer is how a test can send the reactor a control command and then see, from
 * the outside, whether the reactor acted on it. The application thread asks for the timer, the
 * request crosses to the reactor as a ReactorControlCommand, and the reactor creates the
 * descriptor. A command that is lost produces a timer that never fires, which is the failure
 * these tests are looking for.
 *
 * Recurring rather than single-shot, so that a timer which fires late still fires at all and the
 * test is not a race against one chance.
 */
class TimerSchedulingThread : public ApplicationThread {
  public:
    TimerSchedulingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& name, ThreadID id, const QueueConfiguration& qc,
                          const AllocatorConfiguration& ac)
        : ApplicationThread(token, logger, reactor, name, id, qc, ac, ApplicationThreadConfiguration{}) {}

    /** @brief How many timers to ask for. Set before the reactor is run. */
    int timers_wanted{1};

    /**
     * @brief Whether to wait for a message before asking for the timers.
     *
     * This matters more than it looks. on_app_ready_event runs while the reactor is still
     * starting its threads, which is before the reactor reaches its event loop and therefore
     * before it is polling for work. A timer asked for there always takes the wakeup route, so a
     * test that schedules from there is not testing the polling route at all, whatever it says
     * in its name.
     *
     * With this set, the thread asks for its timers when the test sends it a message, which the
     * test does only once the reactor has been running long enough to be inside its polling
     * loop.
     */
    bool schedule_when_messaged{false};

    /** @brief How many distinct timers have fired at least once. */
    [[nodiscard]] size_t distinct_timers_fired() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return fired_.size();
    }

    /** @brief How many timers were successfully asked for. */
    [[nodiscard]] size_t timers_scheduled() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return scheduled_.size();
    }

  protected:
    void on_initial_event() override {}

    void on_app_ready_event() override {
        if (!schedule_when_messaged) {
            ask_for_the_timers();
        }
    }

    void on_timer_event(TimerID id) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        fired_.insert(id.get_value());
    }

    void on_itc_message([[maybe_unused]] const EventMessage& event_message) override {
        if (schedule_when_messaged) {
            ask_for_the_timers();
        }
    }

  private:
    void ask_for_the_timers() {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < timers_wanted; ++i) {
            const TimerID id = schedule_timer(std::chrono::microseconds(1000), TimerType(TimerType::Recurring));
            scheduled_.insert(id.get_value());
        }
    }

    mutable std::mutex mutex_;
    std::set<int> scheduled_;
    std::set<int> fired_;
};

class FakeThread : public ApplicationThread {
  public:
    FakeThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& name, ThreadID id, const QueueConfiguration& qc,
               const AllocatorConfiguration& ac)
        : ApplicationThread(token, logger, reactor, name, id, qc, ac, ApplicationThreadConfiguration{}) {
        set_lifecycle_state(ThreadLifecycleState::Operational);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& event_message) override {}
};

} // namespaces

TEST_F(ReactorTest, InitializationTimeoutTriggersShutdown) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(50);

    // A short shutdown_timeout_ is essential here. finalize_threads_after_shutdown()
    // waits up to shutdown_timeout_ twice -- once for the thread run loop to exit and
    // once for join_with_timeout(). BadThread never stops, so both waits will always
    // exhaust the full timeout. If shutdown_timeout_ is left at its default of 1000ms,
    // finalize_threads_after_shutdown() blocks for ~2000ms, exceeding TearDown's
    // join_reactor_or_die() budget and causing std::terminate().
    cfg.shutdown_timeout_ = std::chrono::milliseconds(100);

    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    auto bad_thread = ApplicationThread::create<NeverStartingThread>(logger_with_sink_.logger, *reactor_, "BadThread", ThreadID(99), make_queue_config(),
                                                                     make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait for Reactor to detect timeout and shut down.
    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();

        while (!reactor_->is_finished()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{200}) {
                FAIL() << "Reactor did not shut down after initialization timeout";
            }
            backoff.pause();
        }
    }

    EXPECT_FALSE(reactor_->is_initialized());
    EXPECT_TRUE(reactor_->is_finished());
}

/*
===============================================================================
 Reactor Init/AppReady Sequencing Test
===============================================================================

This test encodes the lifecycle contract for the Reactor:

  1. Reactor::run() starts all registered ApplicationThreads.
  2. Once Reactor::run() has started, it posts an Initial event to each thread.
  3. Each ApplicationThread processes Initial and marks itself as initialised.
  4. Only after *all* registered threads have processed Initial does the Reactor
     post an AppReady event to each thread.

The test registers two TestApplicationThread instances with the Reactor,
runs the Reactor in its own thread, and then waits until:

  - both threads have seen their Initial event, and
  - both threads have seen their AppReady event.

Assertions inside TestApplicationThread::on_app_ready_event ensure that no
thread can observe AppReady before it has observed Initial. The test as a
whole ensures that the Reactor does not send AppReady to any thread until
all threads have completed their Initial processing.
===============================================================================
*/
TEST_F(ReactorTest, AllThreadsReceiveInitThenAppReady) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);
    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    // Create two test threads
    auto thread1 = ApplicationThread::create<TestApplicationThread>(logger_with_sink_.logger, *reactor_, "thread1", ThreadID{1}, make_queue_config(),
                                                                    make_allocator_config());
    auto thread2 = ApplicationThread::create<TestApplicationThread>(logger_with_sink_.logger, *reactor_, "thread2", ThreadID{2}, make_queue_config(),
                                                                    make_allocator_config());

    reactor_->register_thread(thread1);
    reactor_->register_thread(thread2);

    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    BackoffWithYield backoff;
    while (!reactor_->is_running()) {
        backoff.pause();
    }

    // Wait until both threads have seen their Initial event.
    backoff.reset();
    while (reactor_->is_running() &&
           (!thread1->saw_initial_event.load(std::memory_order_acquire) || !thread2->saw_initial_event.load(std::memory_order_acquire))) {
        backoff.pause();
    }

    EXPECT_TRUE(reactor_->is_running());

    // Now wait until both threads have seen their AppReady event.
    backoff.reset();
    while (reactor_->is_running() &&
           (!thread1->saw_app_ready_event.load(std::memory_order_acquire) || !thread2->saw_app_ready_event.load(std::memory_order_acquire))) {
        backoff.pause();
    }

    EXPECT_TRUE(reactor_->is_running());
    // Final assertions
    EXPECT_TRUE(thread1->saw_initial_event.load());
    EXPECT_TRUE(thread2->saw_initial_event.load());
    EXPECT_TRUE(thread1->saw_app_ready_event.load());
    EXPECT_TRUE(thread2->saw_app_ready_event.load());
}

// Reactor shutdown test: cooperative thread receives Termination event

TEST_F(ReactorTest, ShutdownBroadcastsTerminationAndThreadExits) {
    auto thread = ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, *reactor_, "ShutdownThread", ThreadID{123},
                                                                       make_queue_config(), make_allocator_config());
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait until initialisation completes.
    {
        BackoffWithYield backoff;
        while (!reactor_->is_initialized()) {
            backoff.pause();
        }
    }

    // Trigger shutdown by explicitly shutting the reactor down.
    reactor_->shutdown("test shutdown");

    {
        BackoffWithYield backoff;
        while (!reactor_->is_finished() || thread->is_running()) {
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(thread->is_running());
}

// Reactor shutdown test: thread ignores Termination and never exits

TEST_F(ReactorTest, RogueThreadBlocksInITCMessageReactorStillShutsDown) {
    auto rogue = ApplicationThread::create<RogueITCThread>(logger_with_sink_.logger, *reactor_, "RogueThread", ThreadID{777}, make_queue_config(),
                                                           make_allocator_config());
    reactor_->register_thread(rogue);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait for initialization to complete, give it 2s.
    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();
        while (!reactor_->is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not complete initialization";
            }
            backoff.pause();
        }
    }

    // Send an ITC message to force the rogue thread into its infinite loop.
    {
        const uint8_t dummy_payload[1] = {42};
        EventMessage itc = EventMessage::create_itc_message(rogue->get_thread_id(), dummy_payload, 1);
        rogue->post_message(rogue->get_thread_id(), std::move(itc));
    }

    // Give the rogue thread time to enter on_itc_message().
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Trigger shutdown. Rogue thread will never exit on its own.
    reactor_->shutdown("test shutdown");

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_TRUE(rogue->is_running()); // Rogue thread never exited
}

// A thread that would not stop is recorded, and the process must not return past it.
//
// The reactor's own behaviour is unchanged and deliberately so: it still shuts down despite a rogue
// thread, which is what the test above asserts. What these add is the decision a COMPONENT makes
// afterwards. Returning from main would run the exit handlers and destroy static state -- Quill's
// logger among it -- while the abandoned thread is still executing, which is how a sequencer
// segfaulted on 2026-08-21. See docs/bug_list.md, BUG-0057.
TEST_F(ReactorTest, AbandonedRogueThreadIsRecordedAsStillRunning) {
    auto rogue = ApplicationThread::create<RogueITCThread>(logger_with_sink_.logger, *reactor_, "RogueThread", ThreadID{779}, make_queue_config(),
                                                           make_allocator_config());
    reactor_->register_thread(rogue);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();
        while (!reactor_->is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not complete initialization";
            }
            backoff.pause();
        }
    }

    {
        const uint8_t dummy_payload[1] = {42};
        EventMessage itc = EventMessage::create_itc_message(rogue->get_thread_id(), dummy_payload, 1);
        rogue->post_message(rogue->get_thread_id(), std::move(itc));
    }
    // Long enough that the rogue is certainly inside on_itc_message, which sleeps in 10ms steps.
    // At 20ms this test was intermittently racing the thread into its loop, and when it lost the
    // thread stopped normally -- proving the re-check below rather than the abandonment above.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    reactor_->shutdown("test shutdown");
    ASSERT_TRUE(reactor_thread_->join_with_timeout(std::chrono::seconds(10)));

    // has_exited(), not is_running(): shutdown promotes every thread's lifecycle state to
    // Terminated, so is_running() is false here even for a thread still executing. Asserting on
    // it is what revealed that the abort check had been written against the wrong predicate and
    // could never have fired.
    ASSERT_FALSE(rogue->has_exited()) << "the rogue stopped on its own, so this run tests nothing";
    EXPECT_TRUE(reactor_->abandoned_a_thread());
    // The question a component actually asks. A timeout says how long a thread was given; this
    // says whether it is still running now, which is the only state that makes exiting unsafe.
    EXPECT_TRUE(reactor_->abandoned_thread_still_running());
}

// The case that must stay silent, and the one a profiling run depends on: an ordinary shutdown,
// ended by SIGTERM in production, abandons nothing and the component returns normally.
TEST_F(ReactorTest, CleanShutdownAbandonsNothing) {
    auto thread = ApplicationThread::create<TestApplicationThread>(logger_with_sink_.logger, *reactor_, "well_behaved", ThreadID{781}, make_queue_config(),
                                                                   make_allocator_config());
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();
        while (!reactor_->is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not complete initialization";
            }
            backoff.pause();
        }
    }

    reactor_->shutdown("test shutdown");
    ASSERT_TRUE(reactor_thread_->join_with_timeout(std::chrono::seconds(10)));

    EXPECT_FALSE(reactor_->abandoned_a_thread());
    EXPECT_FALSE(reactor_->abandoned_thread_still_running());
    // Returns rather than aborting. If this ever kills the test binary, every clean shutdown in
    // production aborts too -- including the SIGTERM that ends a perf run.
    reactor_->abort_if_thread_abandoned();
    SUCCEED();
}

TEST_F(ReactorTest, ThreadThrowsDuringTerminationReactorStillShutsDown) {
    auto bad_thread = ApplicationThread::create<ThrowingTerminationThread>(logger_with_sink_.logger, *reactor_, "ThrowingThread", ThreadID{888},
                                                                           make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait for initialization to complete.
    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();
        while (!reactor_->is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not complete initialization";
            }
            backoff.pause();
        }
    }

    // Trigger shutdown. The thread will throw during Termination.
    reactor_->shutdown("test shutdown");

    {
        BackoffWithYield backoff;
        while (!reactor_->is_finished() || bad_thread->is_running()) {
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running()); // Thread must have been shut down
}

// Reactor shutdown test: thread throws during normal message processing

TEST_F(ReactorTest, ThreadThrowsDuringRunLoopReactorShutsDown) {
    auto bad_thread = ApplicationThread::create<ThrowingDuringRunThread>(logger_with_sink_.logger, *reactor_, "ThrowingRunLoopThread", ThreadID{999},
                                                                         make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait until Reactor has started the thread and it is Operational
    for (int i = 0; i < 200 && bad_thread->get_lifecycle_state().as_tag() < ThreadLifecycleState::Operational; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Send an ITC message that will trigger the exception.
    {
        const uint8_t dummy_payload[1] = {42};
        EventMessage itc_message = EventMessage::create_itc_message(bad_thread->get_thread_id(), dummy_payload, 1);
        bad_thread->post_message(bad_thread->get_thread_id(), std::move(itc_message));
    }

    // wait for bad thread to be no longer running
    {
        BackoffWithYield backoff;
        auto start = MillisecondClock::now();
        while (!bad_thread->is_running()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{1000}) {
                FAIL() << "Bad thread still running when should have terminated";
            }
            backoff.pause();
        }
    }

    {
        BackoffWithYield backoff;
        const auto start = MillisecondClock::now();
        while (!reactor_->is_finished()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{1000}) {
                FAIL() << "Reactor did not finish after thread threw";
            }
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor_->is_finished());
}

TEST_F(ReactorTest, ThreadThrowsDuringInitialProcessingReactorShutsDown) {
    auto bad_thread = ApplicationThread::create<ThrowingInitialThread>(logger_with_sink_.logger, *reactor_, "BadInitThread", ThreadID{101}, make_queue_config(),
                                                                       make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running());
}

TEST_F(ReactorTest, ThreadThrowsDuringAppReadyProcessingReactorShutsDown) {
    auto bad_thread = ApplicationThread::create<ThrowingAppReadyThread>(logger_with_sink_.logger, *reactor_, "BadAppReadyThread", ThreadID{202},
                                                                        make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        BackoffWithYield backoff;
        while (!reactor_->is_finished() || bad_thread->is_running()) {
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running());
}

TEST_F(ReactorTest, ReactorRequiresAtLeastOneRegisteredThreadTest) {
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(reactor_->is_finished()); // reactor must immediately finish
    EXPECT_FALSE(reactor_->is_running()); // never entered event loop
}

TEST_F(ReactorTest, RouteMessageBeforeInitializationThrows) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    EventMessage msg = EventMessage::create_itc_message(ThreadID(1), nullptr, 0);

    EXPECT_THROW(reactor.route_message(ThreadID(1), std::move(msg)), PreconditionAssertion); // NOLINT(cppcoreguidelines-avoid-goto,hicpp-avoid-goto)
}

TEST_F(ReactorTest, RouteMessageFromNonRunningOriginIsIgnored) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto target_thread = ApplicationThread::create<NeverStartingThread>(logger_with_sink_.logger, reactor, "TargetThread", ThreadID(1), make_queue_config(),
                                                                        make_allocator_config());

    reactor.register_thread(target_thread);

    // Pretend initialization completed
    reactor.set_lifecycle_state(ReactorLifecycleState::Running);
    reactor.set_initialization_complete(true);

    // Message claims to originate from thread 2, which does not exist
    EventMessage message = EventMessage::create_itc_message(ThreadID(2), nullptr, 0);

    // Should be ignored, not thrown
    reactor.route_message(ThreadID(1), std::move(message));

    EXPECT_TRUE(target_thread->get_queue().empty());
}

TEST_F(ReactorTest, FinalizePromotesShuttingDownToTerminated) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto t = ApplicationThread::create<NeverStartingThread>(logger_with_sink_.logger, reactor, "T", ThreadID(1), make_queue_config(), make_allocator_config());
    reactor.register_thread(t);

    // Force lifecycle state
    t->set_lifecycle_state(ThreadLifecycleState::ShuttingDown);

    reactor.shutdown("x");
    reactor.finalize_threads_after_shutdown();

    EXPECT_EQ(t->get_lifecycle_state().as_tag(), ThreadLifecycleState::Terminated);
}

TEST_F(ReactorTest, CancelTimerWrongOwnerThrows) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto t1 =
        ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, reactor, "A", ThreadID(1), make_queue_config(), make_allocator_config());
    auto t2 =
        ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, reactor, "B", ThreadID(2), make_queue_config(), make_allocator_config());

    reactor.register_thread(t1);
    reactor.register_thread(t2);

    const TimerID tid = reactor.allocate_timer_id();
    reactor.create_timer_fd(tid, ThreadID(1), std::chrono::milliseconds(10), TimerType(TimerType::SingleShot));

    EXPECT_THROW(reactor.cancel_timer_fd(ThreadID(2), tid), PreconditionAssertion); // NOLINT(cppcoreguidelines-avoid-goto,hicpp-avoid-goto)
}

TEST_F(ReactorTest, DispatchEventsUnknownFdIsIgnored) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    epoll_event ev{};
    ev.data.fd = 9999; // bogus FD

    reactor.dispatch_events(1, &ev);
}

TEST_F(ReactorTest, ExitedThreadTriggersShutdown) {
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = ApplicationThread::create<FakeThread>(logger_with_sink_.logger, reactor, "T", ThreadID(1), make_queue_config(), make_allocator_config());
    reactor.register_thread(thread);

    // Force Reactor into Running state
    reactor.set_lifecycle_state(ReactorLifecycleState::Running);

    // Force thread into ShuttingDown state
    thread->set_lifecycle_state(ThreadLifecycleState::ShuttingDown);

    // This should trigger Reactor shutdown
    reactor.check_for_exited_threads();

    EXPECT_TRUE(reactor.is_finished());
}

TEST_F(ReactorTest, StuckOperationalThreadTriggersShutdown) {
    ReactorConfiguration cfg;
    cfg.itc_maximum_inactivity_interval_ = std::chrono::milliseconds(1);
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto thread =
        ApplicationThread::create<FakeThread>(logger_with_sink_.logger, reactor, "ThreadA", ThreadID(1), make_queue_config(), make_allocator_config());

    reactor.register_thread(thread);

    // Reactor must be Running
    reactor.set_lifecycle_state(ReactorLifecycleState::Running);

    // Thread must be Operational
    thread->set_lifecycle_state(ThreadLifecycleState::Operational);

    // Simulate a callback that finished long ago
    auto long_ago = HighResolutionClock::now() - std::chrono::seconds(10);
    thread->set_time_event_started(long_ago);
    thread->set_time_event_finished(HighResolutionClock::now());

    reactor.check_for_stuck_threads();

    EXPECT_TRUE(reactor.is_finished());
}

TEST_F(ReactorTest, GetShutdownReasonReturnsReason) {
    reactor_->shutdown("reason under test");
    EXPECT_EQ(reactor_->get_shutdown_reason(), "reason under test");
}

TEST_F(ReactorTest, GetThreadNameFromIdReturnsName) {
    auto thread = ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, *reactor_, "NamedThread", ThreadID{1}, make_queue_config(),
                                                                       make_allocator_config());
    reactor_->register_thread(thread);

    EXPECT_EQ(reactor_->get_thread_name_from_id(ThreadID{1}), "NamedThread");
}

TEST_F(ReactorTest, HandleSigtermInitiatesShutdown) {
    // handle_sigterm_and_singint() requires the reactor to be in Running state
    // to trigger a shutdown. Set lifecycle state directly via the test seam.
    reactor_->set_lifecycle_state(ReactorLifecycleState::Running);
    reactor_->set_initialization_complete(true);

    reactor_->handle_sigterm_and_singint();

    EXPECT_TRUE(reactor_->is_finished());
}

/*
Asking for CPU pinning and not getting it must be loud. These two tests pin down
that boundary: a deployment that configures pinning but no registry paths refuses
to start, while one that never asked for pinning starts regardless. The failure
these guard against is the quiet one -- warning and running unpinned looks like a
healthy process, so a latency-critical component can lose its cores and nobody
finds out until the numbers are being questioned.
*/

TEST_F(ReactorTest, PinningEnabledWithoutRegistryPathsAbortsStartup) {
    ReactorConfiguration configuration;
    configuration.init_phase_timeout_ = MillisecondClock::duration{2000};
    configuration.shutdown_timeout_ = std::chrono::milliseconds(100);
    configuration.cpu_pinning_enabled = true;
    // cpu_registry_shm_path and cpu_registry_lock_file are left empty deliberately.

    reactor_ = std::make_unique<Reactor>(configuration, service_registry_, logger_with_sink_.logger);
    // A registered thread is needed for startup to reach the pinning step at all.
    auto thread = ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, *reactor_, "PinnedThread", ThreadID{1}, make_queue_config(),
                                                                       make_allocator_config());
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    {
        BackoffWithYield backoff;
        const auto start = MillisecondClock::now();
        while (!reactor_->is_finished()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not shut down when pinning was configured without registry paths";
            }
            backoff.pause();
        }
    }

    EXPECT_FALSE(reactor_->is_initialized());
    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_TRUE(logger_with_sink_.contains_message("CPU pinning is enabled but"));
}

TEST_F(ReactorTest, PinningDisabledWithoutRegistryPathsStartsNormally) {
    ReactorConfiguration configuration;
    configuration.init_phase_timeout_ = MillisecondClock::duration{2000};
    configuration.shutdown_timeout_ = std::chrono::milliseconds(100);
    configuration.cpu_pinning_enabled = false;

    reactor_ = std::make_unique<Reactor>(configuration, service_registry_, logger_with_sink_.logger);
    auto thread = ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink_.logger, *reactor_, "UnpinnedThread", ThreadID{1}, make_queue_config(),
                                                                       make_allocator_config());
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    {
        BackoffWithYield backoff;
        const auto start = MillisecondClock::now();
        while (!reactor_->is_initialized()) {
            if (reactor_->is_finished()) {
                FAIL() << "Reactor shut down instead of initialising with pinning disabled";
            }
            if (MillisecondClock::now() - start > MillisecondClock::duration{2000}) {
                FAIL() << "Reactor did not initialise with pinning disabled";
            }
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor_->is_initialized());
    EXPECT_TRUE(logger_with_sink_.contains_message("CPU pinning disabled"));
}

/*
===============================================================================
 Getting a command from an application thread to the reactor
===============================================================================

An application thread never touches a socket or a timer descriptor. It puts a
ReactorControlCommand on the reactor's queue and the reactor carries it out. Making sure the
reactor looks at that queue is done one of two ways, and which one is used depends on what the
reactor is doing at the time:

  Sleeping in epoll_wait. The only thing that can wake it is a write to its wakeup descriptor,
  so the sending thread makes that write.

  Going round its polling loop. It is already looking at the queue every time round, so the
  sending thread writes nothing and saves two system calls.

The second case is an optimisation of the first, and the danger in it is a command enqueued in
the moment the reactor stops polling: if the sender decides against a wakeup just as the reactor
stops looking, the command waits until something unrelated happens. The three tests below cover
both routes and the changeover between them.

Each test observes the command path through a timer, because a timer is a command whose effect
can be seen from outside the reactor: the application thread asks for one, the request crosses as
a command, and a timer that was asked for and never fires is a command that went missing.
===============================================================================
*/

namespace {

/** @brief Waits for a predicate to come true, and says whether it did. */
bool became_true_within(const std::function<bool()>& predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    BackoffWithYield backoff;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        backoff.pause();
    }
    return predicate();
}

} // namespaces

TEST_F(ReactorTest, PollingReactorFindsACommandWithoutBeingWoken) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);

    // A spin window far longer than the test is prepared to wait. That combination is the whole
    // point: the reactor cannot leave its polling loop during the test, so the only way the
    // timer can be created is the polling loop noticing the command by itself. A reactor that
    // only looked at the queue on the way out of the loop would be caught here, where a longer
    // deadline than the spin window would have let it pass as merely slow.
    cfg.spin_before_block = std::chrono::microseconds{5000000};

    // The reactor's own housekeeping timer is an epoll event like any other, and any epoll event
    // takes the reactor out of its polling loop, which drains the command queue on the way past.
    // Left at its default of one second that alone would carry the command through, and the test
    // would pass with the polling loop's own check removed -- which is exactly what it did until
    // this line was added. Pushed well beyond the length of the test, there is nothing else that
    // can wake this reactor: no sockets, no signals, and no wakeup from the sender, because the
    // sender was told the reactor was polling. The polling loop is then the only way through.
    cfg.inactivity_check_interval_ = std::chrono::seconds{60};
    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = ApplicationThread::create<TimerSchedulingThread>(logger_with_sink_.logger, *reactor_, "timers", ThreadID{1}, make_queue_config(),
                                                                   make_allocator_config());
    thread->timers_wanted = 1;
    thread->schedule_when_messaged = true;
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    // Wait for the reactor to be running and then a little longer, so that it is inside
    // poll_for_work and has told senders so, before the thread is asked for a timer.
    ASSERT_TRUE(became_true_within([this] { return reactor_->is_initialized(); }, std::chrono::milliseconds(2000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    reactor_->route_message(ThreadID{1}, EventMessage::create_itc_message(ThreadID{1}, nullptr, 0));

    // One second against a five second spin window. The timer is asked for with an interval of
    // one millisecond, so a working polling loop has it firing hundreds of times over.
    EXPECT_TRUE(became_true_within([&] { return thread->distinct_timers_fired() == 1; }, std::chrono::milliseconds(1000)))
        << "the timer never fired while the reactor was polling, so the polling loop is not "
           "finding commands by itself -- and no wakeup was sent, because it said it was polling";
    EXPECT_EQ(thread->timers_scheduled(), 1u);
}

TEST_F(ReactorTest, SleepingReactorIsStillWokenByACommand) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);

    // Zero is the default and means the reactor sleeps in epoll_wait as soon as it has nothing
    // to do, so every command here takes the route through the wakeup descriptor. This is the
    // behaviour the polling route must not have broken.
    cfg.spin_before_block = std::chrono::microseconds{0};
    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = ApplicationThread::create<TimerSchedulingThread>(logger_with_sink_.logger, *reactor_, "timers", ThreadID{1}, make_queue_config(),
                                                                   make_allocator_config());
    thread->timers_wanted = 1;
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    EXPECT_TRUE(became_true_within([&] { return thread->distinct_timers_fired() == 1; }, std::chrono::milliseconds(3000)))
        << "the timer never fired, so the wakeup did not reach a sleeping reactor";
    EXPECT_EQ(thread->timers_scheduled(), 1u);
}

TEST_F(ReactorTest, NoCommandIsLostWhileTheReactorKeepsEnteringAndLeavingItsPollingLoop) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);

    // A spin window this short makes the reactor fall out of its polling loop almost at once and
    // go straight back in, over and over. That changeover is the only moment at which a command
    // can be lost: the sender reads "polling" and sends no wakeup while the reactor is on its way
    // out of the loop. Running it constantly is how the test gets many chances to catch it.
    cfg.spin_before_block = std::chrono::microseconds{1};
    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = ApplicationThread::create<TimerSchedulingThread>(logger_with_sink_.logger, *reactor_, "timers", ThreadID{1}, make_queue_config(),
                                                                   make_allocator_config());
    thread->timers_wanted = 64;
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>([this] { reactor_->run(); });

    EXPECT_TRUE(became_true_within([&] { return thread->distinct_timers_fired() == 64; }, std::chrono::milliseconds(5000)))
        << "only " << thread->distinct_timers_fired() << " of 64 timers fired, so a command was lost crossing to the reactor";
    EXPECT_EQ(thread->timers_scheduled(), 64u);
}

/*
===============================================================================
 The reactor's lap: how long between one look for work and the next
===============================================================================

A reactor notices nothing between two calls to epoll_wait. Bytes that arrive from another
process just after one call wait until the following one, so this interval decides how late a
message is seen, and on average a message waits half of it. It is the part of the journey
between two components that no other measurement covers.

The test below does not assert a value, because the right value is a property of the machine
and of what the reactor is carrying. It asserts that the measurement RESPONDS: a reactor told
to spin quietly 4096 times between looks must show a longer lap than one told to spin once,
because that is the one thing known to lengthen a lap. An instrument that reported the same
figure either way would be reporting nothing, and would still have looked perfectly healthy.
===============================================================================
*/

namespace {

/** @brief The mean of a histogram, read back out of a real scrape rather than a fake. */
double mean_from_exposition(const std::string& exposition, const std::string& family) {
    double total = 0.0;
    double count = 0.0;
    std::istringstream stream(exposition);
    std::string line;
    while (std::getline(stream, line)) {
        const auto value_at = line.rfind(' ');
        if (value_at == std::string::npos) {
            continue;
        }
        const std::string value = line.substr(value_at + 1);
        if (line.rfind(family + "_sum", 0) == 0) {
            total = std::stod(value);
        } else if (line.rfind(family + "_count", 0) == 0) {
            count = std::stod(value);
        }
    }
    return count > 0.0 ? total / count : 0.0;
}

/** @brief Runs a polling reactor for a moment and returns its mean lap, in nanoseconds. */
double mean_lap_with_quiet_spins(LoggerWithSink& logger_with_sink, const ServiceRegistry& registry, int32_t quiet_spins) {
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(2000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(200);
    cfg.inactivity_check_interval_ = std::chrono::seconds{60};
    cfg.spin_before_block = std::chrono::microseconds{5000000};
    cfg.quiet_spins_between_polls = quiet_spins;
    cfg.metrics_configuration.enabled = true;
    // Port 0 lets the operating system choose, so parallel test binaries do not collide. The
    // host has to be given as well: the default is empty, which CivetWeb refuses to bind, and
    // the reactor treats a metrics listener it cannot start as fatal.
    cfg.metrics_configuration.listen_endpoint = NetworkEndpointConfiguration{"127.0.0.1", 0};

    Reactor reactor(cfg, registry, logger_with_sink.logger);
    auto thread = ApplicationThread::create<CooperativeShutdownThread>(logger_with_sink.logger, reactor, "idle", ThreadID{1}, make_queue_config(),
                                                                       make_allocator_config());
    reactor.register_thread(thread);

    ThreadWithJoinTimeout reactor_thread([&reactor] { reactor.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string exposition = reactor.metrics().exposition_text();
    reactor.shutdown("lap measurement finished");
    if (!reactor_thread.join_with_timeout(std::chrono::seconds(2))) {
        ADD_FAILURE() << "the reactor thread did not join after shutdown";
    }

    return mean_from_exposition(exposition, "reactor_lap_nanoseconds");
}

} // namespaces

TEST_F(ReactorTest, LapMeasurementRespondsToHowLongTheReactorSpinsBetweenLooks) {
    // Each arm builds a reactor of its own, because the setting under test is fixed when a
    // reactor is constructed. The fixture's reactor is left alone: it is never run, and
    // clearing it would leave TearDown dereferencing nothing.

    const double busy_lap = mean_lap_with_quiet_spins(logger_with_sink_, service_registry_, 1);
    const double idle_lap = mean_lap_with_quiet_spins(logger_with_sink_, service_registry_, 4096);

    ASSERT_GT(busy_lap, 0.0) << "no laps were recorded at all, so the measurement is not running";
    ASSERT_GT(idle_lap, 0.0) << "no laps were recorded at all, so the measurement is not running";

    // 4096 quiet spins against 1. The exact ratio depends on what a quiet spin costs on this
    // processor, so the test asks only for a clear separation rather than a figure.
    EXPECT_GT(idle_lap, busy_lap * 4.0) << "spinning 4096 times between looks gave a mean lap of " << idle_lap << "ns against " << busy_lap
                                        << "ns for spinning once. The lap measurement is not responding to the one "
                                        << "thing known to change it, so it is not measuring what it claims";
}
