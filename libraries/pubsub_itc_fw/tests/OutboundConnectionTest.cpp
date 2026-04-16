// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OutboundConnectionTest.cpp
 * @brief Integration tests for outbound connection error paths.
 *
 * Tests in this file:
 *
 *   ConnectTimeout
 *     The connector is given a service whose primary endpoint is a port that
 *     is not listening (connection attempt will hang until timeout). A very
 *     short connect_timeout is configured. The reactor must detect the timeout
 *     via check_for_timed_out_connections(), tear down the connection, and
 *     deliver ConnectionFailed to the requesting thread. Covers the timeout
 *     branch in OutboundConnectionManager::check_for_timed_out_connections().
 *
 *   SecondaryEndpointRetry
 *     The service is registered with a bad primary endpoint (a port that
 *     immediately refuses the connection) and a good secondary endpoint (a
 *     real listening socket). The reactor must detect the primary failure in
 *     on_connect_ready(), retry the secondary, and ultimately deliver
 *     ConnectionEstablished to the requesting thread. Covers the secondary
 *     retry branch in OutboundConnectionManager::on_connect_ready().
 *
 *   UnknownServiceFails
 *     The connector requests a service name that is not in the ServiceRegistry.
 *     ConnectionFailed must be delivered immediately. Covers the unknown-service
 *     error branch in OutboundConnectionManager::process_connect_command().
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Helpers
// ============================================================

static QueueConfiguration make_queue_config() {
    QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static AllocatorConfiguration make_allocator_config(const std::string& name) {
    AllocatorConfiguration cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

/*
 * Creates a POSIX listening socket on 127.0.0.1:0.
 * Returns {fd, port}. The caller owns the fd and must ::close() it.
 */
static std::pair<int, uint16_t> make_listener() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return {-1, 0};
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {-1, 0};
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return {-1, 0};
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    return {fd, ntohs(bound.sin_port)};
}

// ============================================================
// Application thread that tracks connection outcomes
// ============================================================
class OutboundTestThread : public ApplicationThread {
public:
    OutboundTestThread(QuillLogger& logger, Reactor& reactor,
                       const std::string& service_name)
        : ApplicationThread(logger, reactor, "OutboundTestThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("OutboundTestPool"),
                            ApplicationThreadConfiguration{})
        , service_name_(service_name) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_failed{false};
    std::atomic<bool> connection_lost{false};

    std::string failure_reason;
    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service(service_name_);
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override {
        failure_reason = reason;
        connection_failed.store(true, std::memory_order_release);
        shutdown("connection failed");
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    std::string service_name_;
};

// ============================================================
// Test fixture
// ============================================================
class OutboundConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>("outbound_test_logger", "outbound_test_sink");
    }

    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& t,
                                  const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (t.joinable()) {
            t.join();
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: connect timeout
// ============================================================
TEST_F(OutboundConnectionTest, ConnectTimeout) {
    // Use a port that is not listening so the connect attempt hangs.
    // Port 9 (discard) is almost never bound on loopback in a test environment.
    // We use a non-routable address (192.0.2.1 is TEST-NET, RFC 5737) to
    // guarantee the connect does not complete immediately.
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(50);
    cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
    cfg.connect_timeout            = std::chrono::milliseconds(300);

    ServiceRegistry registry;
    registry.add("slow_service",
        NetworkEndpointConfiguration{"192.0.2.1", 9999}, // TEST-NET -- non-routable
        NetworkEndpointConfiguration{});

    auto reactor = std::make_unique<Reactor>(cfg, registry, logger_->logger);
    auto thread  = std::make_shared<OutboundTestThread>(
        logger_->logger, *reactor, "slow_service");
    reactor->register_thread(thread);

    std::thread reactor_thread([&]() { reactor->run(); });

    // Allow up to 3 seconds: 300ms timeout + housekeeping interval margin.
    EXPECT_TRUE(wait_for([&]() {
        return thread->connection_failed.load(std::memory_order_acquire);
    }, 3000)) << "ConnectionFailed not delivered after connect timeout";

    EXPECT_FALSE(thread->connection_established.load(std::memory_order_acquire));
    EXPECT_FALSE(thread->failure_reason.empty());

    shutdown_and_join(*reactor, reactor_thread);
}

// ============================================================
// Test: secondary endpoint retry
// ============================================================
TEST_F(OutboundConnectionTest, SecondaryEndpointRetry) {
    // Start a real listener for the secondary endpoint.
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1) << "Failed to create listening socket";

    // Find a port that is guaranteed to refuse connections by binding and
    // immediately closing a socket without calling listen(). The kernel will
    // send RST to anyone connecting, giving us a clean synchronous refusal
    // that reliably triggers the primary-failed / retry-secondary path.
    uint16_t refused_port = 0;
    {
        const int tmp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_NE(tmp_fd, -1);
        const int one = 1;
        ::setsockopt(tmp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in tmp_addr{};
        tmp_addr.sin_family      = AF_INET;
        tmp_addr.sin_port        = 0;
        tmp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        ::bind(tmp_fd, reinterpret_cast<sockaddr*>(&tmp_addr), sizeof(tmp_addr));
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(tmp_fd, reinterpret_cast<sockaddr*>(&bound), &len);
        refused_port = ntohs(bound.sin_port);
        ::close(tmp_fd); // closed without listen() -- connections will be refused
    }
    ASSERT_NE(refused_port, 0u);

    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
    cfg.connect_timeout            = std::chrono::milliseconds(2000);

    ServiceRegistry registry;
    registry.add("my_service",
        NetworkEndpointConfiguration{"127.0.0.1", refused_port},
        NetworkEndpointConfiguration{"127.0.0.1", listen_port});

    auto reactor = std::make_unique<Reactor>(cfg, registry, logger_->logger);
    auto thread  = std::make_shared<OutboundTestThread>(
        logger_->logger, *reactor, "my_service");
    reactor->register_thread(thread);

    std::thread reactor_thread([&]() { reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return thread->connection_established.load(std::memory_order_acquire)
            || thread->connection_failed.load(std::memory_order_acquire);
    })) << "Neither ConnectionEstablished nor ConnectionFailed received";

    EXPECT_TRUE(thread->connection_established.load(std::memory_order_acquire))
        << "Expected ConnectionEstablished via secondary endpoint, got failure: "
        << thread->failure_reason;

    ::close(listen_fd);
    shutdown_and_join(*reactor, reactor_thread);
}

// ============================================================
// Test: unknown service name delivers ConnectionFailed immediately
// ============================================================
TEST_F(OutboundConnectionTest, UnknownServiceFails) {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
    cfg.connect_timeout            = std::chrono::milliseconds(2000);

    ServiceRegistry registry; // empty -- no services registered

    auto reactor = std::make_unique<Reactor>(cfg, registry, logger_->logger);
    auto thread  = std::make_shared<OutboundTestThread>(
        logger_->logger, *reactor, "no_such_service");
    reactor->register_thread(thread);

    std::thread reactor_thread([&]() { reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return thread->connection_failed.load(std::memory_order_acquire);
    })) << "ConnectionFailed not delivered for unknown service";

    EXPECT_FALSE(thread->connection_established.load(std::memory_order_acquire));
    EXPECT_FALSE(thread->failure_reason.empty());

    shutdown_and_join(*reactor, reactor_thread);
}

} // namespace pubsub_itc_fw::tests
