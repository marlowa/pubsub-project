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

namespace pubsub_itc_fw {

// -----------------------------------------------------------------------------
// Reactor initialization timeout test
// -----------------------------------------------------------------------------

namespace {

// ------------------------------------------------------------
// Helpers: QueueConfig, AllocatorConfig
// ------------------------------------------------------------
QueueConfig make_queue_config()
{
    QueueConfig cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 3;
    cfg.for_client_use = nullptr;
    cfg.gone_below_low_watermark_handler = nullptr;
    cfg.gone_above_high_watermark_handler = nullptr;
    return cfg;
}

AllocatorConfig make_allocator_config()
{
    AllocatorConfig cfg{};
    cfg.pool_name = "ATestPool";
    cfg.objects_per_pool = 128;
    cfg.initial_pools = 1;
    cfg.expansion_threshold_hint = 0;
    cfg.handler_for_pool_exhausted = nullptr;
    cfg.handler_for_invalid_free = nullptr;
    cfg.handler_for_huge_pages_error = nullptr;
    cfg.use_huge_pages_flag = UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages);
    cfg.context = nullptr;
    return cfg;
}

// A deliberately misbehaving thread that never reaches Started.
// It launches a thread but run_internal() never sets the lifecycle state.
class NeverStartingThread : public pubsub_itc_fw::ApplicationThread {
public:
    NeverStartingThread(pubsub_itc_fw::QuillLogger& logger,
                        pubsub_itc_fw::Reactor& reactor,
                        const std::string& name,
                        pubsub_itc_fw::ThreadID id,
                        const pubsub_itc_fw::QueueConfig& qc,
                        const pubsub_itc_fw::AllocatorConfig& ac)
        : ApplicationThread(logger, reactor, name, id, qc, ac)
    {}

protected:
    void on_initial_event() override
    {
        // Intentionally never calls set_lifecycle_state(Started).
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void on_itc_message(const EventMessage& msg) override {
    }
};

} // namespace

TEST(ReactorTest, InitializationTimeoutTriggersShutdown)
{
    pubsub_itc_fw::LoggerWithSink logger("reactor_timeout_logger",
                                         "reactor_timeout_sink");

    pubsub_itc_fw::ReactorConfiguration cfg{};
    cfg.init_phase_timeout_ = pubsub_itc_fw::MillisecondClock::duration{50};

    pubsub_itc_fw::Reactor reactor(cfg, logger.logger);

    pubsub_itc_fw::QueueConfig qc = make_queue_config();
    pubsub_itc_fw::AllocatorConfig ac = make_allocator_config();

    auto bad_thread = std::make_shared<NeverStartingThread>(
        logger.logger, reactor, "BadThread", pubsub_itc_fw::ThreadID(99), qc, ac);

    reactor.register_thread(bad_thread);

    pubsub_itc_fw::ThreadWithJoinTimeout reactor_thread([&]() {
        reactor.run();
    });

    // Wait for Reactor to detect timeout and shut down.
    {
        pubsub_itc_fw::Backoff backoff;
        auto start = pubsub_itc_fw::MillisecondClock::now();

        while (!reactor.is_finished()) {
            if (pubsub_itc_fw::MillisecondClock::now() - start >
                pubsub_itc_fw::MillisecondClock::duration{200}) {
                FAIL() << "Reactor did not shut down after initialization timeout";
            }
            backoff.pause();
        }
    }

    reactor_thread.join();

    EXPECT_FALSE(reactor.is_initialized());
    EXPECT_TRUE(reactor.is_finished());
}

} // namespaces

// -----------------------------------------------------------------------------
// A test ApplicationThread that records Init and AppReady events.
// -----------------------------------------------------------------------------
class TestApplicationThread : public pubsub_itc_fw::ApplicationThread {
public:
    TestApplicationThread(pubsub_itc_fw::QuillLogger& logger,
                          pubsub_itc_fw::Reactor& reactor,
                          const std::string& name,
                          pubsub_itc_fw::ThreadID id,
                          const pubsub_itc_fw::QueueConfig& queue_config,
                          const pubsub_itc_fw::AllocatorConfig& allocator_config)
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

    void on_itc_message(const pubsub_itc_fw::EventMessage&) override {
        // Not used in this test.
    }
};

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
    using namespace pubsub_itc_fw;

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

    // -------------------------------------------------------------------------
    // Run the Reactor in its own thread (it will block until shutdown).
    // -------------------------------------------------------------------------
    std::thread reactor_thread([&reactor]() {
        reactor.run();
    });

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

    // -------------------------------------------------------------------------
    // Clean shutdown for the test.
    // -------------------------------------------------------------------------
    reactor.shutdown("test shutdown");
    reactor_thread.join();
}
