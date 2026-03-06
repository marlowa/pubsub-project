// ReactorInitAppReadyTest.cpp

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Reactor.hpp>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/EventType.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/MisbehavingThreads.hpp>

// -----------------------------------------------------------------------------
// Reactor initialization timeout test
// -----------------------------------------------------------------------------

using namespace pubsub_itc_fw;

namespace {

// TODO these are copied from ApplicationThreadTest.

// ------------------------------------------------------------
// Helpers: QueueConfig, AllocatorConfig
// ------------------------------------------------------------
pubsub_itc_fw::QueueConfig make_queue_config()
{
    pubsub_itc_fw::QueueConfig cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 3;
    cfg.for_client_use = nullptr;
    cfg.gone_below_low_watermark_handler = nullptr;
    cfg.gone_above_high_watermark_handler = nullptr;
    return cfg;
}

pubsub_itc_fw::AllocatorConfig make_allocator_config()
{
    pubsub_itc_fw::AllocatorConfig cfg{};
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

// A cooperative thread that does nothing special at all
class CooperativeShutdownThread : public ApplicationThread {
public:
    CooperativeShutdownThread(QuillLogger& logger,
                              Reactor& reactor,
                              const std::string& name,
                              ThreadID id,
                              const QueueConfig& qc,
                              const AllocatorConfig& ac)
        : ApplicationThread(logger, reactor, name, id, qc, ac)
    {}

protected:
    void on_initial_event() override {
        // Nothing special for this test.
    }

    void on_app_ready_event() override {
        // Nothing special for this test.
    }

    void on_itc_message(const EventMessage& msg) override {
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
                          const QueueConfig& queue_config,
                          const AllocatorConfig& allocator_config)
        : ApplicationThread(logger, reactor, name, id, queue_config, allocator_config)
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

    void on_itc_message(const EventMessage&) override {
        // Not used in this test.
    }
};

} // un-named namespace

void join_reactor_or_die(ThreadWithJoinTimeout& reactor_thread, std::chrono::milliseconds timeout)
{
    if (!reactor_thread.join_with_timeout(timeout)) {
        std::cerr << "FATAL: reactor thread did not join (timeout " << timeout.count() << " ms)\n";
        std::terminate();
    }
}

TEST(ReactorTest, InitializationTimeoutTriggersShutdown)
{
    using namespace test_support;
    LoggerWithSink logger("reactor_timeout_logger", "reactor_timeout_sink");

    ReactorConfiguration cfg{};
    cfg.init_phase_timeout_ = MillisecondClock::duration{50};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(50);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(50);
    Reactor reactor(cfg, logger.logger);

    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    auto bad_thread = std::make_shared<NeverStartingThread>(
        logger.logger, reactor, "BadThread", ThreadID(99), qc, ac);

    reactor.register_thread(bad_thread);

    ThreadWithJoinTimeout reactor_thread([&]() {
        reactor.run();
    });

    // Wait for Reactor to detect timeout and shut down.
    {
        Backoff backoff;
        auto start = MillisecondClock::now();

        while (!reactor.is_finished()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{200}) {
                FAIL() << "Reactor did not shut down after initialization timeout";
            }
            backoff.pause();
        }
    }

    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_FALSE(reactor.is_initialized());
    EXPECT_TRUE(reactor.is_finished());
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
TEST(ReactorTest, AllThreadsReceiveInitThenAppReady) {

    // -------------------------------------------------------------------------
    // Configuration setup
    // -------------------------------------------------------------------------
    ReactorConfiguration reactor_config{};
    QueueConfig queue_config = make_queue_config();
    AllocatorConfig allocator_config = make_allocator_config();

    auto logger = std::make_unique<LoggerWithSink>("ReactorTestLogger", "ReactorTestSink");
    Reactor reactor(reactor_config, logger->logger);

    // -------------------------------------------------------------------------
    // Create two test threads
    // -------------------------------------------------------------------------
    auto thread1 = std::make_shared<TestApplicationThread>(
        logger->logger, reactor, "thread1", ThreadID{1}, queue_config, allocator_config);
    auto thread2 = std::make_shared<TestApplicationThread>(
        logger->logger, reactor, "thread2", ThreadID{2}, queue_config, allocator_config);

    reactor.register_thread(thread1);
    reactor.register_thread(thread2);

    ThreadWithJoinTimeout reactor_thread([&]() { reactor.run(); });

    // -------------------------------------------------------------------------
    // Wait until both threads have seen their Initial event.
    // -------------------------------------------------------------------------
    Backoff backoff;
    while (!thread1->saw_initial_event.load(std::memory_order_acquire) ||
           !thread2->saw_initial_event.load(std::memory_order_acquire)) {
        backoff.pause();
    }

    // -------------------------------------------------------------------------
    // Now wait until both threads have seen their AppReady event.
    // -------------------------------------------------------------------------
    while (!thread1->saw_app_ready_event.load(std::memory_order_acquire) ||
           !thread2->saw_app_ready_event.load(std::memory_order_acquire)) {
        backoff.pause();
    }

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

TEST(ReactorTest, ShutdownBroadcastsTerminationAndThreadExits)
{
    LoggerWithSink logger("reactor_shutdown_logger", "reactor_shutdown_sink");

    ReactorConfiguration cfg{};
    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    Reactor reactor(cfg, logger.logger);

    auto thread = std::make_shared<CooperativeShutdownThread>(
        logger.logger, reactor, "ShutdownThread", ThreadID{123}, qc, ac);

    reactor.register_thread(thread);
    ThreadWithJoinTimeout reactor_thread([&]() { reactor.run(); });

    // Wait until initialization completes.
    {
        Backoff backoff;
        while (!reactor.is_initialized()) {
            backoff.pause();
        }
    }

    // Trigger shutdown by explicitly shutting the reactor down.
    reactor.shutdown("test shutdown");

    // Reactor should exit its event loop and return from run().
    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_TRUE(reactor.is_finished());
    EXPECT_FALSE(thread->is_running());
}

// -----------------------------------------------------------------------------
// Reactor shutdown test: thread ignores Termination and never exits
// -----------------------------------------------------------------------------

TEST(ReactorTest, RogueThreadBlocksInITCMessage_ReactorStillShutsDown)
{
    using namespace test_support;

    LoggerWithSink logger("reactor_shutdown_rogue_logger", "reactor_shutdown_rogue_sink");

    ReactorConfiguration cfg{};
    cfg.shutdown_timeout_ = MillisecondClock::duration{100};  // Keep test fast

    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    Reactor reactor(cfg, logger.logger);

    auto rogue = std::make_shared<RogueITCThread>(
        logger.logger, reactor, "RogueThread", ThreadID{777}, qc, ac);

    reactor.register_thread(rogue);
    ThreadWithJoinTimeout reactor_thread([&]() {
        reactor.run();
    });

    // Wait for initialization to complete.
    {
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (!reactor.is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{500}) {
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
    reactor.shutdown("test shutdown");

    // Reactor::shutdown() must return even though the thread is hung.
    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_TRUE(reactor.is_finished());
    EXPECT_TRUE(rogue->is_running());  // Rogue thread never exited
}

TEST(ReactorTest, ThreadThrowsDuringTerminationReactorStillShutsDown)
{
    using namespace test_support;

    LoggerWithSink logger("reactor_shutdown_throw_logger", "reactor_shutdown_throw_sink");

    ReactorConfiguration cfg{};
    cfg.shutdown_timeout_ = MillisecondClock::duration{100};  // Keep test fast

    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    Reactor reactor(cfg, logger.logger);

    auto bad_thread = std::make_shared<ThrowingTerminationThread>(
        logger.logger, reactor, "ThrowingThread", ThreadID{888}, qc, ac);

    reactor.register_thread(bad_thread);
    ThreadWithJoinTimeout reactor_thread([&]() {
        reactor.run();
    });

    // Wait for initialization to complete.
    {
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (!reactor.is_initialized()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{500}) {
                FAIL() << "Reactor did not complete initialization";
            }
            backoff.pause();
        }
    }

    // Trigger shutdown. The thread will throw during Termination.
    reactor.shutdown("test shutdown");

    // Reactor::shutdown() must return even though the thread threw.
    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_TRUE(reactor.is_finished());
    EXPECT_FALSE(bad_thread->is_running());  // Thread must have been shut down
}

// -----------------------------------------------------------------------------
// Reactor shutdown test: thread throws during normal message processing
// -----------------------------------------------------------------------------

TEST(ReactorTest, ThreadThrowsDuringRunLoopReactorShutsDown)
{
    using namespace test_support;

    LoggerWithSink logger("reactor_shutdown_runloop_logger", "reactor_shutdown_runloop_sink");

    ReactorConfiguration reactor_config{};
    reactor_config.shutdown_timeout_ = MillisecondClock::duration{100};

    QueueConfig thread_queue_config = make_queue_config();
    AllocatorConfig thread_allocator_config = make_allocator_config();

    Reactor reactor(reactor_config, logger.logger);

    auto bad_thread = std::make_shared<ThrowingDuringRunThread>(logger.logger, reactor,
        "ThrowingRunLoopThread", ThreadID{999}, thread_queue_config, thread_allocator_config);

    reactor.register_thread(bad_thread);
    auto reactor_thread = std::make_unique<ThreadWithJoinTimeout>( [&reactor] { reactor.run(); });

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
        Backoff backoff;
        auto start = MillisecondClock::now();
        while (!bad_thread->is_running()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{1000}) {
                FAIL() << "Bad thread still running when should have terminated";
            }
            backoff.pause();
        }
    }

    {
        Backoff backoff;
        auto const start = MillisecondClock::now();
        while (!reactor.is_finished()) {
            if (MillisecondClock::now() - start > MillisecondClock::duration{1000}) {
                FAIL() << "Reactor did not finish after thread threw";
            }
            backoff.pause();
        }
    }

    EXPECT_TRUE(reactor.is_finished());
}

TEST(ReactorTest, ThreadThrowsDuringInitialProcessingReactorShutsDown)
{
    using namespace test_support;

    LoggerWithSink logger("reactor_init_throw_logger", "reactor_init_throw_sink");

    ReactorConfiguration cfg{};
    cfg.init_phase_timeout_ = MillisecondClock::duration{200};
    cfg.shutdown_timeout_   = MillisecondClock::duration{200};

    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    Reactor reactor(cfg, logger.logger);

    auto bad_thread = std::make_shared<ThrowingInitialThread>(logger.logger, reactor, "BadInitThread", ThreadID{101}, qc, ac);

    reactor.register_thread(bad_thread);
    ThreadWithJoinTimeout reactor_thread([&]() { reactor.run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_TRUE(reactor.is_finished());
    EXPECT_EQ(bad_thread->get_lifecycle_state().as_tag(), ThreadLifecycleState::Terminated);
}

TEST(ReactorTest, ThreadThrowsDuringAppReadyProcessingReactorShutsDown)
{
    using namespace test_support;

    LoggerWithSink logger("reactor_appready_throw_logger", "reactor_appready_throw_sink");

    ReactorConfiguration reactor_config{};
    reactor_config.init_phase_timeout_ = MillisecondClock::duration{200};
    reactor_config.shutdown_timeout_   = MillisecondClock::duration{200};

    QueueConfig thread_queue_config = make_queue_config();
    AllocatorConfig thread_allocator_config = make_allocator_config();

    Reactor reactor(reactor_config, logger.logger);

    auto bad_thread = std::make_shared<ThrowingAppReadyThread>(
        logger.logger,
        reactor,
        "BadAppReadyThread",
        ThreadID{202},
        thread_queue_config,
        thread_allocator_config);

    reactor.register_thread(bad_thread);

    ThreadWithJoinTimeout reactor_thread([&]() { reactor.run(); });

    // Give the reactor some time to start things up and send the init event.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    join_reactor_or_die(reactor_thread, std::chrono::milliseconds(500));

    EXPECT_TRUE(reactor.is_finished());
    EXPECT_EQ(bad_thread->get_lifecycle_state().as_tag(), ThreadLifecycleState::Terminated);
}
