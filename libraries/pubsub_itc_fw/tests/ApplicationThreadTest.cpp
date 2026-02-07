#include <atomic>
#include <thread>

#include <gtest/gtest.h>           // Google Test framework

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/MockLogger.hpp>
#include <pubsub_itc_fw/tests_common/MockReactor.hpp>

namespace pubsub_itc_fw::tests {

class TestApplicationThread : public ApplicationThread {
public:
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<int> processed_messages{0};

    TestApplicationThread(const LoggerInterface& logger,
                          Reactor& reactor,
                          const std::string& name,
                          ThreadID id)
        : ApplicationThread(logger, reactor, name, id, 0, 100) {}

    void run() override {
        running = true;

        while (!stop_requested.load()) {
            auto msg = get_queue().dequeue();
            if (msg.has_value()) {
                process_message(*msg);
            } else {
                std::this_thread::yield();
            }
        }

        running = false;
    }

    void process_message(EventMessage& message) override {
        processed_messages++;
    }
};

TEST(ApplicationThreadTest, StartPostProcessAndShutdown) {
    MockLogger logger;
    MockReactor reactor(logger);

    TestApplicationThread app(logger, reactor, "test-thread", ThreadID{1});

    // Start the thread
    app.start();

    // Wait until the thread is running
    while (!app.running.load()) {
        std::this_thread::yield();
    }

    uint8_t dummy_payload[1] = {0};

    EventMessage msg = EventMessage::create_itc_message(ThreadID{1}, dummy_payload, 0);

    // Post the message
    app.post_message("test-thread", msg);

    // Wait for processing
    while (app.processed_messages.load() == 0) {
        std::this_thread::yield();
    }

    EXPECT_EQ(app.processed_messages.load(), 1);

    // Request the thread to stop
    app.stop_requested = true;

    // Trigger shutdown
    app.shutdown("test shutdown");

    // Join the thread
    bool joined = app.join_with_timeout(std::chrono::milliseconds(500));
    EXPECT_TRUE(joined);

    // Reactor shutdown should have been called
    EXPECT_TRUE(reactor.shutdown_called.load());
    EXPECT_EQ(reactor.shutdown_reason, "test shutdown");
}

} // namespaces
