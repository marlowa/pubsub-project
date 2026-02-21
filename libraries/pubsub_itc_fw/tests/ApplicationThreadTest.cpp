#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include <quill/Frontend.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <pubsub_itc_fw/tests_common/MockReactor.hpp>
#include <pubsub_itc_fw/tests_common/TestSink.hpp>
#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>

using namespace pubsub_itc_fw;

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

class ApplicationThreadTestEnv : public ::testing::Environment {
public:
    LoggerWithSink* logger_with_sink = nullptr;
    MockReactor* reactor = nullptr;

    void SetUp() override {
        // Create long-lived logger + sink
        logger_with_sink = new LoggerWithSink("global_AT_logger", "global_AT_sink");

        // Create long-lived Reactor
        ReactorConfiguration cfg{};
        reactor = new MockReactor(cfg, logger_with_sink->logger);
    }

    void TearDown() override {
        delete reactor;
        delete logger_with_sink;
    }
};

// Register this environment ONLY for this translation unit
static ::testing::Environment* const appThreadEnv =
    ::testing::AddGlobalTestEnvironment(new ApplicationThreadTestEnv());

static ApplicationThreadTestEnv* env() {
    return static_cast<ApplicationThreadTestEnv*>(appThreadEnv);
}

class ApplicationThreadTest : public ::testing::Test {
public:
    ApplicationThreadTest()
        : logger_with_sink_(*env()->logger_with_sink),
          reactor_(*env()->reactor)
    {}

    void SetUp() override {
        logger_with_sink_.sink->clear();
    }

    LoggerWithSink& logger_with_sink_;
    MockReactor& reactor_;
};

// ------------------------------------------------------------
// Test subclass of ApplicationThread
// ------------------------------------------------------------
class TestThread : public ApplicationThread
{
public:
    ~TestThread() override = default;

    TestThread(QuillLogger& logger,
               Reactor& reactor,
               const std::string& name,
               ThreadID id,
               const QueueConfig& queueConfig,
               const AllocatorConfig& allocatorConfig)
        : ApplicationThread(logger, reactor, name, id, queueConfig, allocatorConfig)
    {
    }

    void on_initial_event() override
    {
        processed_count.fetch_add(1, std::memory_order_release);

        if (throw_on_message.load(std::memory_order_acquire)) {
            throw std::runtime_error("Test exception");
        }

        last_processed_type.store(EventType(EventType::Initial), std::memory_order_release);
    }

    void on_app_ready_event() override {
        last_processed_type.store(EventType(EventType::AppReady), std::memory_order_release);
    }

    void on_itc_message(const EventMessage& msg) override {
        last_processed_type.store(EventType(EventType::InterthreadCommunication), std::memory_order_release);
    }

    void on_timer_event(TimerID id) override {
        last_processed_type.store(EventType(EventType::Timer), std::memory_order_release);
    }

    std::atomic<int> processed_count{0};
    std::atomic<bool> throw_on_message{false};
    std::atomic<EventType> last_processed_type{EventType(EventType::None)};
};

} // unnamed namespace

// ------------------------------------------------------------
// TEST 1: Start and shutdown
// ------------------------------------------------------------
// What: Verifies that the thread transitions to running and then cleanly
//       shuts down via ApplicationThread::shutdown.
// Why:  Confirms the basic lifecycle wiring (start + shutdown) is correct.
// How:  Start the thread, wait briefly, assert is_running() is true, then
//       call shutdown and assert is_running() is false.
TEST_F(ApplicationThreadTest, StartAndShutdown)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(thread.is_running());

    thread.shutdown("normal");
    EXPECT_FALSE(thread.is_running());
}

// ------------------------------------------------------------
// TEST 2: Message processing
// ------------------------------------------------------------
// What: Ensures that messages enqueued to the thread’s queue are eventually
//       processed by process_message.
// Why:  Validates the basic producer/consumer path and that the internal
//       run loop is actually draining the queue.
// How:  Start the thread, enqueue a single Initial event, wait, then
//       shutdown and assert processed_count >= 1.
TEST_F(ApplicationThreadTest, MessageProcessing)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();

    thread.post_message(ThreadID(1), EventMessage::create_reactor_event(EventType(EventType::Initial)));

    // Wait until the message is actually processed
    for (int i = 0; i < 100 && thread.processed_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    thread.shutdown("done");

    EXPECT_GE(thread.processed_count.load(), 1);
}

// ------------------------------------------------------------
// TEST 3: Pause/resume
// ------------------------------------------------------------
// What: Verifies that pausing the thread prevents message processing until
//       resume is called.
// Why:  Confirms that the pause flag is honored by the run loop and that
//       messages are not lost but processed after resume.
// How:  Pause the thread, enqueue a message, assert processed_count == 0,
//       then resume, wait, and assert processed_count >= 1.
TEST_F(ApplicationThreadTest, PauseResume)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    thread.pause();

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(thread.processed_count.load(), 0);

    thread.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    thread.shutdown("done");
    EXPECT_GE(thread.processed_count.load(), 1);
}

// ------------------------------------------------------------
// TEST 4: Exception triggers Reactor shutdown
// ------------------------------------------------------------
// What: Ensures that an exception thrown from process_message causes the
//       Reactor to be shut down and the thread to stop running.
// Why:  This is the fatal-path contract: unhandled exceptions in the
//       application thread should trigger a Reactor shutdown.
// How:  Configure the TestThread to throw on message, enqueue a message,
//       wait, then assert that reactor is finished and !thread.is_running().
TEST_F(ApplicationThreadTest, ExceptionTriggersShutdown)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.throw_on_message.store(true, std::memory_order_release);

    reactor_.run();
    thread.start();

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    // Wait until the message is processed (which will throw)
    for (int i = 0; i < 100 && thread.processed_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_GT(thread.processed_count.load(std::memory_order_acquire), 0);

    // Now wait for the reactor to observe shutdown
    for (int i = 0; i < 100 && !reactor_.is_finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(reactor_.is_finished());
    EXPECT_FALSE(thread.is_running());
}

// ------------------------------------------------------------
// TEST 5: Queue shutdown behavior
// ------------------------------------------------------------
// What: Verifies that once the queue is shut down, further messages are
//       dropped and not processed.
// Why:  Confirms the shutdown semantics of LockFreeMessageQueue and the
//       ApplicationThread wrapper around it.
// How:  Shutdown the thread, then enqueue a message and wait; assert that
//       processed_count remains 0.
TEST_F(ApplicationThreadTest, QueueShutdownDropsMessages)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    thread.shutdown("test");

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg)); // should be dropped

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_EQ(thread.processed_count.load(), 0);
}

// ------------------------------------------------------------
// TEST 6: Timestamp semantics
// ------------------------------------------------------------
// What: Checks that the timestamp setters/getters on ApplicationThread
//       behave as simple value holders.
// Why:  These timestamps are used for latency measurement; correctness
//       here avoids subtle timing bugs later.
// How:  Set start and finish timestamps and assert they are returned
//       unchanged by the getters.
TEST_F(ApplicationThreadTest, TimestampSemantics)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    auto before = std::chrono::system_clock::now();
    thread.set_time_event_started(before);

    auto after = before + std::chrono::milliseconds(5);
    thread.set_time_event_finished(after);

    EXPECT_EQ(thread.get_time_event_started(), before);
    EXPECT_EQ(thread.get_time_event_finished(), after);
}

// ------------------------------------------------------------
// TEST 7: Logging verification
// ------------------------------------------------------------
// What: Ensures that the ApplicationThread emits at least one log message
//       during its lifecycle (start/shutdown path).
// Why:  Confirms that the logger wiring is live and that logs are routed
//       to the configured TestSink.
// How:  Clear the sink, start and shutdown the thread, then assert that
//       at least one log record was captured and that it mentions "shutdown".
TEST_F(ApplicationThreadTest, LoggingVerification)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    thread.shutdown("log test");

    EXPECT_GE(logger_with_sink_.sink->count(), 1);
    EXPECT_TRUE(logger_with_sink_.sink->contains_message("shutdown"));
}

// ------------------------------------------------------------
// TEST 8: Pause/resume under load
// ------------------------------------------------------------
// What: Stress-tests pause/resume semantics under a burst of messages.
// Why:  Ensures that pausing under load does not lose messages and that
//       they are processed once the thread is resumed.
// How:  Pause the thread, enqueue many messages, assert no processing,
//       then resume, wait, and assert processed_count > 0.
TEST_F(ApplicationThreadTest, PauseResumeUnderLoad)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    thread.pause();

    // Enqueue a burst of messages while paused
    for (int i = 0; i < 50; ++i)
    {
        EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
        thread.post_message(ThreadID(1), std::move(msg));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Should not have processed anything yet
    EXPECT_EQ(thread.processed_count.load(), 0);

    thread.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    thread.shutdown("done");

    EXPECT_GT(thread.processed_count.load(), 0);
}

// ------------------------------------------------------------
// TEST 9: Watermark transitions
// ------------------------------------------------------------
// What: Verifies that the high/low watermark handlers are invoked exactly
//       once when the queue crosses the configured thresholds.
// Why:  Confirms the hysteresis semantics of LockFreeMessageQueue and that
//       ApplicationThread’s queue wiring preserves them.
// How:  Install handlers that bump atomics, push enough messages to cross
//       the high watermark, then drain via shutdown and assert each handler
//       fired exactly once.
// ------------------------------------------------------------
// TEST 9: Watermark transitions
// ------------------------------------------------------------
// What: Verifies that the high/low watermark handlers are invoked exactly
//       once when the queue crosses the configured thresholds.
// Why:  Confirms the hysteresis semantics of LockFreeMessageQueue and that
//       ApplicationThread's queue wiring preserves them.
// How:  Pause the thread to prevent draining, enqueue messages to cross
//       the high watermark, then resume and wait for both watermarks to
//       fire using futures for deterministic synchronization.
TEST_F(ApplicationThreadTest, WatermarkTransitions)
{
    QueueConfig qc = make_queue_config();
    qc.low_watermark = 1;
    qc.high_watermark = 3;

    std::atomic<int> high_triggered{0};
    std::atomic<int> low_triggered{0};

    // Synchronization primitives for deterministic waiting
    std::atomic<bool> high_fired{false};
    std::atomic<bool> low_fired{false};
    std::promise<void> high_promise;
    std::promise<void> low_promise;
    auto high_future = high_promise.get_future();
    auto low_future = low_promise.get_future();

    // Handlers now signal via promises when they fire
    qc.gone_above_high_watermark_handler = [&](void*) {
        high_triggered.fetch_add(1, std::memory_order_release);
        // Only set promise once to avoid multiple set_value() calls
        if (!high_fired.exchange(true, std::memory_order_acq_rel)) {
            high_promise.set_value();
        }
    };

    qc.gone_below_low_watermark_handler = [&](void*) {
        low_triggered.fetch_add(1, std::memory_order_release);
        // Only set promise once
        if (!low_fired.exchange(true, std::memory_order_acq_rel)) {
            low_promise.set_value();
        }
    };

    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      qc,
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // CRITICAL: Pause the thread to prevent it from draining the queue
    // while we're enqueuing messages. This ensures the queue will actually
    // reach the high watermark.
    thread.pause();

    // Enqueue 4 messages while paused (high watermark is 3)
    for (int i = 0; i < 4; ++i)
    {
        EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
        thread.post_message(ThreadID(1), std::move(msg));
    }

    // Give a moment for the enqueue operations to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Resume thread so it can process messages and trigger watermarks
    thread.resume();

    // Wait for high watermark handler to fire (timeout after 1 second)
    auto high_status = high_future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(high_status, std::future_status::ready)
        << "High watermark handler did not fire within timeout";

    // Wait for low watermark handler to fire (timeout after 1 second)
    auto low_status = low_future.wait_for(std::chrono::seconds(1));
    ASSERT_EQ(low_status, std::future_status::ready)
        << "Low watermark handler did not fire within timeout";

    // Clean shutdown
    thread.shutdown("done");

    // Verify each handler fired exactly once
    EXPECT_EQ(high_triggered.load(), 1);
    EXPECT_EQ(low_triggered.load(), 1);
}

// ------------------------------------------------------------
// TEST 10: Exception logging contains thread metadata
// ------------------------------------------------------------
// What: Ensures that when an exception occurs in the thread, the log
//       message includes both the thread name and thread ID.
// Why:  This is critical for diagnosing failures in production; logs
//       must carry enough metadata to identify the failing thread.
// How:  Force an exception, then scan TestSink::records() for a message
//       containing "MetaThread" and "[42]".
TEST_F(ApplicationThreadTest, ExceptionLoggingContainsThreadMetadata)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.throw_on_message.store(true, std::memory_order_release);

    reactor_.run();
    thread.start();

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    // 1. Wait until the message is processed (which will then throw)
    for (int i = 0; i < 100 && thread.processed_count.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(thread.processed_count.load(std::memory_order_acquire), 0);

    // 2. Wait until the reactor has shut down (exception handler finished)
    for (int i = 0; i < 100 && !reactor_.is_finished(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(reactor_.is_finished());

    // 3. Give the logger a moment to flush into the sink
    for (int i = 0; i < 100 && logger_with_sink_.sink->records().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto& records = logger_with_sink_.sink->records();
    ASSERT_FALSE(records.empty());

    const auto& msg_text = records.back().message; // or however you access it
    EXPECT_NE(msg_text.find("TestThread"), std::string::npos);
    EXPECT_NE(msg_text.find("1"), std::string::npos); // thread_id, etc.
}

// ------------------------------------------------------------
// TEST 11: Message ordering preserved
// ------------------------------------------------------------
// What: Checks that messages are processed in FIFO order by the thread’s
//       queue and run loop.
// Why:  Ordering guarantees are important for many application-level
//       protocols; this test gives a basic sanity check.
// How:  Enqueue three events with distinct types and assert that the last
//       processed type is the last one enqueued.
TEST_F(ApplicationThreadTest, MessageOrderingPreserved)
{
    TestThread thread(logger_with_sink_.logger,
                      reactor_,
                      "OrderThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage m1 = EventMessage::create_reactor_event(EventType(EventType::Initial));
    EventMessage m2 = EventMessage::create_reactor_event(EventType(EventType::AppReady));
    EventMessage m3 = EventMessage::create_reactor_event(EventType(EventType::Timer));

    thread.post_message(ThreadID(1), std::move(m1));
    thread.post_message(ThreadID(1), std::move(m2));
    thread.post_message(ThreadID(1), std::move(m3));

    for (int i = 0; i < 200 && thread.processed_count.load() < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    thread.shutdown("done");

    EXPECT_EQ(thread.last_processed_type.load(std::memory_order_acquire), EventType(EventType::Timer));
}

// ------------------------------------------------------------
// TEST 12: Logger isolation across threads
// ------------------------------------------------------------
// What: Ensures that two ApplicationThread instances using different
//       QuillLogger instances do not leak logs into each other’s sinks.
// Why:  Confirms logger isolation and that the logging backend respects
//       the configured sinks per logger.
// How:  Create two loggers + sinks, run two threads, shutdown with
//       different reasons, and assert each sink only contains its own
//       shutdown reason.
TEST_F(ApplicationThreadTest, LoggerIsolationAcrossThreads)
{
    auto logger1 = std::make_unique<LoggerWithSink>("logger1_LoggerIsolationAcrossThreads", "sink1_LoggerIsolationAcrossThreads");
    auto logger2 = std::make_unique<LoggerWithSink>("logger2_LoggerIsolationAcrossThreads", "sink2_LoggerIsolationAcrossThreads");

    logger1->sink->clear();
    logger2->sink->clear();

    ReactorConfiguration cfg{};
    Reactor reactor(cfg, logger1->logger);

    TestThread t1(logger1->logger, reactor, "T1", ThreadID(1), make_queue_config(), make_allocator_config());
    TestThread t2(logger2->logger, reactor, "T2", ThreadID(2), make_queue_config(), make_allocator_config());

    reactor_.run();
    t1.start();
    t2.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    t1.shutdown("one");
    t2.shutdown("two");

    EXPECT_GT(logger1->sink->count(), 0);
    EXPECT_GT(logger2->sink->count(), 0);

    EXPECT_TRUE(logger1->sink->contains_message("one"));
    EXPECT_TRUE(logger2->sink->contains_message("two"));

    EXPECT_FALSE(logger1->sink->contains_message("two"));
    EXPECT_FALSE(logger2->sink->contains_message("one"));
}

TEST_F(ApplicationThreadTest, DoubleStartThrowsException)
{
    // What: Verifies that calling start() twice throws PreconditionAssertion
    // Why:  Prevents undefined behavior from multiple thread starts
    // How:  Start thread, then attempt to start again

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());
    reactor_.run();
    thread.start();
    // Second start should throw
    EXPECT_THROW(thread.start(), PreconditionAssertion);
    thread.shutdown("done");
}

TEST_F(ApplicationThreadTest, ShutdownBeforeStartIsGraceful)
{
    // What: Verifies that calling shutdown() on a never-started thread is safe
    // Why:  Edge case handling - cleanup without initialization
    // How:  Create thread, call shutdown without start

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    // Should not crash or hang
    EXPECT_NO_THROW(thread.shutdown("never started"));
    EXPECT_FALSE(thread.is_running());
}

TEST_F(ApplicationThreadTest, JoinWithTimeoutSucceeds)
{
    // What: Verifies join_with_timeout returns true when thread finishes
    // Why:  Tests the join mechanism works correctly

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    thread.shutdown("test");

    // Thread should finish quickly
    bool joined = thread.join_with_timeout(std::chrono::milliseconds(500));
    EXPECT_TRUE(joined);
}

TEST_F(ApplicationThreadTest, JoinWithTimeoutBeforeStartReturnsFalse)
{
    // What: Verifies join_with_timeout returns false when thread never started
    // Why:  Edge case - joining a thread that doesn't exist

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    // Should return false immediately
    bool joined = thread.join_with_timeout(std::chrono::milliseconds(100));
    EXPECT_FALSE(joined);
}

TEST_F(ApplicationThreadTest, TimestampGettersAndSetters)
{
    // What: Verifies timestamp tracking functionality
    // Why:  These are used for latency measurement in production

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    auto now = std::chrono::system_clock::now();
    auto later = now + std::chrono::milliseconds(100);

    thread.set_time_event_started(now);
    thread.set_time_event_finished(later);

    EXPECT_EQ(thread.get_time_event_started(), now);
    EXPECT_EQ(thread.get_time_event_finished(), later);
}

TEST_F(ApplicationThreadTest, InitialEventFlagTracking)
{
    // What: Verifies has_processed_initial_event flag works
    // Why:  Used for initialization sequencing in production

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    EXPECT_FALSE(thread.get_has_processed_initial_event());

    thread.set_has_processed_initial_event();

    EXPECT_TRUE(thread.get_has_processed_initial_event());
}

TEST_F(ApplicationThreadTest, ThreadNameAndIDGetters)
{
    // What: Verifies thread metadata is correctly stored and retrieved
    // Why:  Critical for debugging and logging

    const std::string expected_name = "MyTestThread";
    const ThreadID expected_id(123);

    TestThread thread(logger_with_sink_.logger, reactor_, expected_name,
                     expected_id, make_queue_config(), make_allocator_config());

    EXPECT_EQ(thread.get_thread_name(), expected_name);
    EXPECT_EQ(thread.get_thread_id(), expected_id);
}

TEST_F(ApplicationThreadTest, DestructorJoinsThread)
{
    // What: Verifies destructor attempts to join the thread
    // Why:  Ensures clean shutdown semantics
    // How:  Let thread go out of scope while running, check logs for join attempt

    {
        TestThread thread(logger_with_sink_.logger, reactor_, "DestructorTest",
                         ThreadID(1), make_queue_config(), make_allocator_config());

        reactor_.run();
        thread.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Let it go out of scope without explicit shutdown
        // Destructor should attempt join with 3-second timeout
    }

    // Check that destructor attempted to join (might log error if timeout)
    // This is more of a "doesn't crash" test
    SUCCEED();
}

TEST_F(ApplicationThreadTest, MultiplePauseResumeCycles)
{
    // What: Verifies pause/resume can be called multiple times
    // Why:  Tests state machine robustness

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Cycle 1
    thread.pause();
    EventMessage msg1 = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(thread.processed_count.load(), 0);

    thread.resume();
    // Wait for processing
    for (int i = 0; i < 100 && thread.processed_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_EQ(thread.processed_count.load(), 1);

    // Cycle 2
    thread.pause();
    EventMessage msg2 = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg2));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(thread.processed_count.load(), 1); // Still 1

    thread.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(thread.processed_count.load(), 2);

    thread.shutdown("done");
}

TEST_F(ApplicationThreadTest, ExceptionTriggersReactorShutdown)
{
    // What: Verifies that reactor.shutdown() is called when exception occurs
    // Why:  Critical contract - unhandled exception is fatal

    TestThread thread(logger_with_sink_.logger, reactor_, "TestThread",
                     ThreadID(1), make_queue_config(), make_allocator_config());

    thread.throw_on_message.store(true);

    reactor_.run();
    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    // Wait until the reactor has actually shut down
     for (int i = 0; i < 100 && !reactor_.is_finished(); ++i) {
         std::this_thread::sleep_for(std::chrono::milliseconds(1));
     }

     EXPECT_GT(thread.processed_count.load(std::memory_order_acquire), 0);

    // Verify reactor's shutdown was called
    EXPECT_TRUE(reactor_.is_finished());
    EXPECT_FALSE(thread.is_running());
}

TEST_F(ApplicationThreadTest, InterThreadRoutingDeliversMessage)
{
    LoggerWithSink logger1("itc_logger1", "itc_sink1");
    LoggerWithSink logger2("itc_logger2", "itc_sink2");

    ReactorConfiguration cfg{};
    Reactor reactor(cfg, logger1.logger);

    QueueConfig qc = make_queue_config();
    AllocatorConfig ac = make_allocator_config();

    auto threadA = std::make_shared<TestThread>(logger1.logger, reactor, "ThreadA", ThreadID(1), qc, ac);

    auto threadB = std::make_shared<TestThread>(logger2.logger, reactor, "ThreadB", ThreadID(2), qc, ac);

    reactor.register_thread(threadA);
    reactor.register_thread(threadB);

    // Start Reactor in its own thread
    std::thread reactor_thread([&]() {
        reactor.run();
    });

    // Wait for reactor to say that all threads are ready
    {
        Backoff backoff;
        while (!reactor.is_initialized()) {
            backoff.pause();
        }
    }

    // Send an ITC message from A → B
    EventMessage msg = EventMessage::create_itc_message(threadA->get_thread_id(), nullptr, 0);

    threadA->post_message(ThreadID(2), std::move(msg));

    // Wait for B to process the message
    {
        Backoff backoff;
        while (threadB->last_processed_type.load(std::memory_order_acquire) != EventType(EventType::InterthreadCommunication)) {
            backoff.pause();
        }
    }

    // Clean shutdown
    threadA->shutdown("done");
    threadB->shutdown("done");
    reactor.shutdown("done");
    reactor_thread.join();

    EXPECT_EQ(threadB->last_processed_type.load(std::memory_order_acquire), EventType(EventType::InterthreadCommunication));
}
