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
    EventType last_processed_type{EventType::None};
};

} // unnamed namespace

// ------------------------------------------------------------
// TEST 1: Start and shutdown
// ------------------------------------------------------------
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
