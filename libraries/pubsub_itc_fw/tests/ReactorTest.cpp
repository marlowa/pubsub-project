// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>
#include <memory>

#include <sys/epoll.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Reactor.hpp>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BackoffWithYield.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/HighResolutionClock.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/MisbehavingThreads.hpp>

// -----------------------------------------------------------------------------
// Reactor initialization timeout test
// -----------------------------------------------------------------------------

using namespace pubsub_itc_fw;
using namespace test_support;

namespace {

// TODO these are copied from ApplicationThreadTest.

// ------------------------------------------------------------
// Helpers: QueueConfiguration, AllocatorConfiguration
// ------------------------------------------------------------
pubsub_itc_fw::QueueConfiguration make_queue_config()
{
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 3;
    cfg.for_client_use = nullptr;
    cfg.gone_below_low_watermark_handler = nullptr;
    cfg.gone_above_high_watermark_handler = nullptr;
    return cfg;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config()
{
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
        logger_with_sink = new LoggerWithSink("global_REACT_logger", "global_REACT_sink");
    }

    void TearDown() override {
        delete logger_with_sink;
    }
};

// Register this environment ONLY for this translation unit
static ::testing::Environment* const reactorEnv =
    ::testing::AddGlobalTestEnvironment(new ReactorTestEnv());

static ReactorTestEnv* env() {
    return static_cast<ReactorTestEnv*>(reactorEnv);
}

class ReactorTest : public ::testing::Test
{
public:
    ReactorTest()
        : logger_with_sink_(*env()->logger_with_sink) {
    }

    void SetUp() override {
        logger_with_sink_.sink->clear();
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

    void join_reactor_or_die(std::chrono::milliseconds timeout) const
    {
        ASSERT_TRUE(reactor_thread_); // if this fails, it’s a test bug

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
    CooperativeShutdownThread(QuillLogger& logger,
                              Reactor& reactor,
                              const std::string& name,
                              ThreadID id,
                              const QueueConfiguration& qc,
                              const AllocatorConfiguration& ac)
        : ApplicationThread(logger, reactor, name, id, qc, ac, ApplicationThreadConfiguration{})
    {}

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

// -----------------------------------------------------------------------------
// A test ApplicationThread that records Init and AppReady events.
// -----------------------------------------------------------------------------
class TestApplicationThread : public ApplicationThread {
public:
    TestApplicationThread(QuillLogger& logger,
                          Reactor& reactor,
                          const std::string& name,
                          ThreadID id,
                          const QueueConfiguration& queue_config,
                          const AllocatorConfiguration& allocator_config)
        : ApplicationThread(logger, reactor, name, id, queue_config, allocator_config, ApplicationThreadConfiguration{})
    {}

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

    void on_itc_message([[maybe_unused]] const EventMessage& eventMessage) override {
        // Not used in this test.
    }
};

class FakeThread : public ApplicationThread {
public:
    FakeThread(QuillLogger& logger,
               Reactor& reactor,
               const std::string& name,
               ThreadID id,
               const QueueConfiguration& qc,
               const AllocatorConfiguration& ac)
        : ApplicationThread(logger, reactor, name, id, qc, ac, ApplicationThreadConfiguration{})
    {
        set_lifecycle_state(ThreadLifecycleState::Operational);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& eventMessage) override {}
};

} // un-named namespace

TEST_F(ReactorTest, InitializationTimeoutTriggersShutdown)
{
    ReactorConfiguration cfg;
    cfg.init_phase_timeout_ = std::chrono::milliseconds(50);

    // A short shutdown_timeout_ is essential here. finalize_threads_after_shutdown()
    // waits up to shutdown_timeout_ twice — once for the thread run loop to exit and
    // once for join_with_timeout(). BadThread never stops, so both waits will always
    // exhaust the full timeout. If shutdown_timeout_ is left at its default of 1000ms,
    // finalize_threads_after_shutdown() blocks for ~2000ms, exceeding TearDown's
    // join_reactor_or_die() budget and causing std::terminate().
    cfg.shutdown_timeout_    = std::chrono::milliseconds(100);

    reactor_ = std::make_unique<Reactor>(cfg, service_registry_, logger_with_sink_.logger);

    auto bad_thread = std::make_shared<NeverStartingThread>(logger_with_sink_.logger, *reactor_,
                                                            "BadThread", ThreadID(99),
                                                            make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

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

    // -------------------------------------------------------------------------
    // Create two test threads
    // -------------------------------------------------------------------------
    auto thread1 = std::make_shared<TestApplicationThread>(logger_with_sink_.logger, *reactor_,
        "thread1", ThreadID{1}, make_queue_config(), make_allocator_config());
    auto thread2 = std::make_shared<TestApplicationThread>(logger_with_sink_.logger, *reactor_,
        "thread2", ThreadID{2}, make_queue_config(), make_allocator_config());

    reactor_->register_thread(thread1);
    reactor_->register_thread(thread2);

    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

    BackoffWithYield backoff;
    while (!reactor_->is_running()) {
        backoff.pause();
    }

    // -------------------------------------------------------------------------
    // Wait until both threads have seen their Initial event.
    // -------------------------------------------------------------------------
    backoff.reset();
    while (reactor_->is_running() && (
               !thread1->saw_initial_event.load(std::memory_order_acquire) ||
               !thread2->saw_initial_event.load(std::memory_order_acquire))) {
        backoff.pause();
    }

    EXPECT_TRUE(reactor_->is_running());

    // -------------------------------------------------------------------------
    // Now wait until both threads have seen their AppReady event.
    // -------------------------------------------------------------------------
    backoff.reset();
    while (reactor_->is_running() && (
               !thread1->saw_app_ready_event.load(std::memory_order_acquire) ||
               !thread2->saw_app_ready_event.load(std::memory_order_acquire))) {
        backoff.pause();
    }

    EXPECT_TRUE(reactor_->is_running());
    // -------------------------------------------------------------------------
    // Final assertions
    // -------------------------------------------------------------------------
    EXPECT_TRUE(thread1->saw_initial_event.load());
    EXPECT_TRUE(thread2->saw_initial_event.load());
    EXPECT_TRUE(thread1->saw_app_ready_event.load());
    EXPECT_TRUE(thread2->saw_app_ready_event.load());
}

// -----------------------------------------------------------------------------
// Reactor shutdown test: cooperative thread receives Termination event
// -----------------------------------------------------------------------------

TEST_F(ReactorTest, ShutdownBroadcastsTerminationAndThreadExits)
{
    auto thread = std::make_shared<CooperativeShutdownThread>(logger_with_sink_.logger, *reactor_,
                              "ShutdownThread", ThreadID{123}, make_queue_config(), make_allocator_config());
    reactor_->register_thread(thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

    // Wait until initialization completes.
    {
        BackoffWithYield backoff;
        while (!reactor_->is_initialized()) {
            backoff.pause();
        }
    }

    // Trigger shutdown by explicitly shutting the reactor down.
    reactor_->shutdown("test shutdown");

    { BackoffWithYield backoff; while (!reactor_->is_finished() || thread->is_running()) { backoff.pause(); } }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(thread->is_running());
}

// -----------------------------------------------------------------------------
// Reactor shutdown test: thread ignores Termination and never exits
// -----------------------------------------------------------------------------

TEST_F(ReactorTest, RogueThreadBlocksInITCMessageReactorStillShutsDown)
{
    auto rogue = std::make_shared<RogueITCThread>(logger_with_sink_.logger, *reactor_,
                                                  "RogueThread", ThreadID{777},
                                                  make_queue_config(), make_allocator_config());
    reactor_->register_thread(rogue);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

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
        EventMessage itc = EventMessage::create_itc_message(
            rogue->get_thread_id(), dummy_payload, 1);
        rogue->post_message(rogue->get_thread_id(), std::move(itc));
    }

    // Give the rogue thread time to enter on_itc_message().
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Trigger shutdown. Rogue thread will never exit on its own.
    reactor_->shutdown("test shutdown");

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_TRUE(rogue->is_running());  // Rogue thread never exited
}

TEST_F(ReactorTest, ThreadThrowsDuringTerminationReactorStillShutsDown)
{
    auto bad_thread = std::make_shared<ThrowingTerminationThread>(logger_with_sink_.logger, *reactor_,
                                                                  "ThrowingThread", ThreadID{888},
                                                            make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

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

    { BackoffWithYield backoff; while (!reactor_->is_finished() || bad_thread->is_running()) { backoff.pause(); } }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running());  // Thread must have been shut down
}

// -----------------------------------------------------------------------------
// Reactor shutdown test: thread throws during normal message processing
// -----------------------------------------------------------------------------

TEST_F(ReactorTest, ThreadThrowsDuringRunLoopReactorShutsDown)
{
    auto bad_thread = std::make_shared<ThrowingDuringRunThread>(logger_with_sink_.logger, *reactor_,
                                                                "ThrowingRunLoopThread", ThreadID{999},
                                                                make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

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

TEST_F(ReactorTest, ThreadThrowsDuringInitialProcessingReactorShutsDown)
{
    auto bad_thread = std::make_shared<ThrowingInitialThread>(logger_with_sink_.logger, *reactor_,
                                                              "BadInitThread", ThreadID{101},
                                                            make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running());
}

TEST_F(ReactorTest, ThreadThrowsDuringAppReadyProcessingReactorShutsDown)
{
    auto bad_thread = std::make_shared<ThrowingAppReadyThread>(logger_with_sink_.logger, *reactor_,
                                                               "BadAppReadyThread", ThreadID{202},
                                                               make_queue_config(), make_allocator_config());
    reactor_->register_thread(bad_thread);
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    { BackoffWithYield backoff; while (!reactor_->is_finished() || bad_thread->is_running()) { backoff.pause(); } }

    EXPECT_TRUE(reactor_->is_finished());
    EXPECT_FALSE(bad_thread->is_running());
}

TEST_F(ReactorTest, ReactorRequiresAtLeastOneRegisteredThreadTest)
{
    reactor_thread_ = std::make_unique<ThreadWithJoinTimeout>( [this] { reactor_->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(reactor_->is_finished()); // reactor must immediately finish
    EXPECT_FALSE(reactor_->is_running()); // never entered event loop
}

TEST_F(ReactorTest, RouteMessageBeforeInitializationThrows)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    EventMessage msg = EventMessage::create_itc_message(ThreadID(1), nullptr, 0);

    EXPECT_THROW(reactor.route_message(ThreadID(1), std::move(msg)), PreconditionAssertion); // NOLINT(cppcoreguidelines-avoid-goto,hicpp-avoid-goto)
}

TEST_F(ReactorTest, RouteMessageFromNonRunningOriginIsIgnored)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto target_thread = std::make_shared<NeverStartingThread>(
        logger_with_sink_.logger, reactor,
        "TargetThread", ThreadID(1),
        make_queue_config(), make_allocator_config());

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

TEST_F(ReactorTest, FinalizePromotesShuttingDownToTerminated)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto t = std::make_shared<NeverStartingThread>(logger_with_sink_.logger, reactor,
                                                   "T", ThreadID(1),
                                                   make_queue_config(), make_allocator_config());
    reactor.register_thread(t);

    // Force lifecycle state
    t->set_lifecycle_state(ThreadLifecycleState::ShuttingDown);

    reactor.shutdown("x");
    reactor.finalize_threads_after_shutdown();

    EXPECT_EQ(t->get_lifecycle_state().as_tag(), ThreadLifecycleState::Terminated);
}

TEST_F(ReactorTest, CancelTimerWrongOwnerThrows)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto t1 = std::make_shared<CooperativeShutdownThread>(logger_with_sink_.logger, reactor,
                                                          "A", ThreadID(1),
                                                          make_queue_config(), make_allocator_config());
    auto t2 = std::make_shared<CooperativeShutdownThread>(logger_with_sink_.logger, reactor,
                                                          "B", ThreadID(2),
                                                          make_queue_config(), make_allocator_config());

    reactor.register_thread(t1);
    reactor.register_thread(t2);

    const TimerID tid = reactor.allocate_timer_id();
    reactor.create_timer_fd(tid, "X", ThreadID(1), std::chrono::milliseconds(10), TimerType::SingleShot);

    EXPECT_THROW(reactor.cancel_timer_fd(ThreadID(2), tid), PreconditionAssertion); // NOLINT(cppcoreguidelines-avoid-goto,hicpp-avoid-goto)
}

TEST_F(ReactorTest, DispatchEventsUnknownFdIsIgnored)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    epoll_event ev{};
    ev.data.fd = 9999; // bogus FD

    reactor.dispatch_events(1, &ev);
}

TEST_F(ReactorTest, ExitedThreadTriggersShutdown)
{
    const ReactorConfiguration cfg;
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = std::make_shared<FakeThread>(logger_with_sink_.logger, reactor,
                                          "T", ThreadID(1), make_queue_config(), make_allocator_config());
    reactor.register_thread(thread);

    // Force Reactor into Running state
    reactor.set_lifecycle_state(ReactorLifecycleState::Running);

    // Force thread into ShuttingDown state
    thread->set_lifecycle_state(ThreadLifecycleState::ShuttingDown);

    // This should trigger Reactor shutdown
    reactor.check_for_exited_threads();

    EXPECT_TRUE(reactor.is_finished());
}

TEST_F(ReactorTest, StuckOperationalThreadTriggersShutdown)
{
    ReactorConfiguration cfg;
    cfg.itc_maximum_inactivity_interval_ = std::chrono::milliseconds(1);
    Reactor reactor(cfg, service_registry_, logger_with_sink_.logger);

    auto thread = std::make_shared<FakeThread>(
        logger_with_sink_.logger, reactor,
        "ThreadA", ThreadID(1),
        make_queue_config(), make_allocator_config());

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

TEST_F(ReactorTest, GetShutdownReasonReturnsReason)
{
    reactor_->shutdown("reason under test");
    EXPECT_EQ(reactor_->get_shutdown_reason(), "reason under test");
}

TEST_F(ReactorTest, GetThreadNameFromIdReturnsName)
{
    auto thread = std::make_shared<CooperativeShutdownThread>(
        logger_with_sink_.logger, *reactor_,
        "NamedThread", ThreadID{1},
        make_queue_config(), make_allocator_config());
    reactor_->register_thread(thread);

    EXPECT_EQ(reactor_->get_thread_name_from_id(ThreadID{1}), "NamedThread");
}

TEST_F(ReactorTest, HandleSigtermInitiatesShutdown)
{
    // handle_sigterm_and_singint() requires the reactor to be in Running state
    // to trigger a shutdown. Set lifecycle state directly via the test seam.
    reactor_->set_lifecycle_state(ReactorLifecycleState::Running);
    reactor_->set_initialization_complete(true);

    reactor_->handle_sigterm_and_singint();

    EXPECT_TRUE(reactor_->is_finished());
}
