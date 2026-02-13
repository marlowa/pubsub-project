#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

#include <quill/Frontend.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/tests_common/TestSink.hpp>
#include <pubsub_itc_fw/tests_common/MockReactor.hpp>
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

// ------------------------------------------------------------
// Helper: QuillLogger + TestSink
// ------------------------------------------------------------
struct LoggerWithSink
{
    QuillLogger logger;
    std::shared_ptr<TestSink> sink;
};

LoggerWithSink make_logger_with_sink()
{
    LoggerWithSink out{QuillLogger{}, nullptr};

    auto sink_base = quill::Frontend::create_or_get_sink<TestSink>("app_thread_test_sink");
    auto sink_typed = std::static_pointer_cast<TestSink>(sink_base);

    // NOTE: This relies on the real QuillLogger having a ctor that matches this signature
    // in your actual codebase.
    out.logger = QuillLogger("ApplicationThreadTest_logger", sink_base, LogLevel::Debug);
    out.sink = sink_typed;
    return out;
}

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
               const QueueConfig& qc,
               const AllocatorConfig& ac)
        : ApplicationThread(logger, reactor, name, id, qc, ac)
    {
    }

    void run() override
    {
        // Delegate to the framework’s internal loop so we exercise the real
        // ApplicationThread behavior (logging, shutdown, exception handling).
        run_internal();
    }

    void process_message(EventMessage& msg) override
    {
        processed_count.fetch_add(1, std::memory_order_release);

        if (throw_on_message.load(std::memory_order_acquire)) {
            throw std::runtime_error("Test exception");
        }

        last_processed_type = msg.type();
    }

    std::atomic<int> processed_count{0};
    std::atomic<bool> throw_on_message{false};
    EventType last_processed_type{EventType(EventType::None)};
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
TEST(ApplicationThreadTest, StartAndShutdown)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

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
TEST(ApplicationThreadTest, MessageProcessing)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
TEST(ApplicationThreadTest, PauseResume)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

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
//       wait, then assert reactor.shutdown_called() and !thread.is_running().
TEST(ApplicationThreadTest, ExceptionTriggersShutdown)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.throw_on_message.store(true, std::memory_order_release);

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    EXPECT_TRUE(reactor.shutdown_called());
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
TEST(ApplicationThreadTest, QueueShutdownDropsMessages)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

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
TEST(ApplicationThreadTest, TimestampSemantics)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
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
TEST(ApplicationThreadTest, LoggingVerification)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    logger_with_sink.sink->clear();

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    thread.shutdown("log test");

    EXPECT_GE(logger_with_sink.sink->count(), 1);
    EXPECT_TRUE(logger_with_sink.sink->contains_message("shutdown"));
}

// ------------------------------------------------------------
// TEST 8: Pause/resume under load
// ------------------------------------------------------------
// What: Stress-tests pause/resume semantics under a burst of messages.
// Why:  Ensures that pausing under load does not lose messages and that
//       they are processed once the thread is resumed.
// How:  Pause the thread, enqueue many messages, assert no processing,
//       then resume, wait, and assert processed_count > 0.
TEST(ApplicationThreadTest, PauseResumeUnderLoad)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

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
TEST(ApplicationThreadTest, WatermarkTransitions)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    QueueConfig qc = make_queue_config();
    qc.low_watermark = 1;
    qc.high_watermark = 3;

    std::atomic<int> high_triggered{0};
    std::atomic<int> low_triggered{0};

    // Handlers are std::function<void(void*)>, so accept a void* context.
    qc.gone_above_high_watermark_handler = [&](void*) { high_triggered++; };
    qc.gone_below_low_watermark_handler = [&](void*) { low_triggered++; };

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "TestThread",
                      ThreadID(1),
                      qc,
                      make_allocator_config());

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Push above high watermark
    for (int i = 0; i < 4; ++i)
    {
        EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
        thread.post_message(ThreadID(1), std::move(msg));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Drain queue via shutdown
    thread.shutdown("done");

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
TEST(ApplicationThreadTest, ExceptionLoggingContainsThreadMetadata)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    logger_with_sink.sink->clear();

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "MetaThread",
                      ThreadID(42),
                      make_queue_config(),
                      make_allocator_config());

    thread.throw_on_message.store(true);

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage msg = EventMessage::create_reactor_event(EventType(EventType::Initial));
    thread.post_message(ThreadID(1), std::move(msg));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const auto& records = logger_with_sink.sink->records();

    bool found_name = false;
    bool found_id = false;

    for (const auto& r : records)
    {
        const auto& s = r.message;
        if (s.find("MetaThread") != std::string::npos) found_name = true;
        if (s.find("[42]") != std::string::npos)       found_id = true;
    }

    EXPECT_TRUE(found_name);
    EXPECT_TRUE(found_id);
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
TEST(ApplicationThreadTest, MessageOrderingPreserved)
{
    auto logger_with_sink = make_logger_with_sink();
    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger_with_sink.logger);

    TestThread thread(logger_with_sink.logger,
                      reactor,
                      "OrderThread",
                      ThreadID(1),
                      make_queue_config(),
                      make_allocator_config());

    thread.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EventMessage m1 = EventMessage::create_reactor_event(EventType(EventType::Initial));
    EventMessage m2 = EventMessage::create_reactor_event(EventType(EventType::AppReady));
    EventMessage m3 = EventMessage::create_reactor_event(EventType(EventType::Timer));

    thread.post_message(ThreadID(1), std::move(m1));
    thread.post_message(ThreadID(1), std::move(m2));
    thread.post_message(ThreadID(1), std::move(m3));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    thread.shutdown("done");

    EXPECT_EQ(thread.last_processed_type, EventType(EventType::Timer));
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
TEST(ApplicationThreadTest, LoggerIsolationAcrossThreads)
{
    auto logger1 = make_logger_with_sink();
    auto logger2 = make_logger_with_sink();

    logger1.sink->clear();
    logger2.sink->clear();

    ReactorConfiguration cfg{};
    MockReactor reactor(cfg, logger1.logger);

    TestThread t1(logger1.logger, reactor, "T1", ThreadID(1), make_queue_config(), make_allocator_config());
    TestThread t2(logger2.logger, reactor, "T2", ThreadID(2), make_queue_config(), make_allocator_config());

    t1.start();
    t2.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    t1.shutdown("one");
    t2.shutdown("two");

    EXPECT_GT(logger1.sink->count(), 0);
    EXPECT_GT(logger2.sink->count(), 0);

    EXPECT_TRUE(logger1.sink->contains_message("one"));
    EXPECT_TRUE(logger2.sink->contains_message("two"));

    EXPECT_FALSE(logger1.sink->contains_message("two"));
    EXPECT_FALSE(logger2.sink->contains_message("one"));
}
