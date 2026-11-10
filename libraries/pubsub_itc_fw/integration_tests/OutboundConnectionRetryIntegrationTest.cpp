// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OutboundConnectionRetryIntegrationTest.cpp
 * @brief Integration test: retry_failed_connections re-issues a connect after timeout.
 *
 * A non-routable address (192.0.2.1, TEST-NET) causes connect_timeout to fire.
 * check_for_timed_out_connections() tears down the connection and calls schedule_retry(),
 * which populates pending_retries_. On the next housekeeping tick,
 * retry_failed_connections() calls process_connect_command() again. We count
 * connection_failed events: >=2 proves the retry path executed.
 *
 * The tests after it are for ApplicationThread::connect_now(), which makes a waiting attempt at once rather than when the
 * retry interval runs out (docs/availability/reconnecting_to_a_restarted_engine.md, BUG-0237), and for the order a
 * component's start-up relies on with it: a connection a thread asks for in its initial event is made only once the
 * reactor's own listeners are bound. The retry interval in those tests is a minute, so a connection made within a
 * second was not made by a retry.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;
namespace pubsub_itc_fw {

// Application thread that counts connection failures without
// shutting down the reactor, so the retry path can fire.
class RetryCountingThread : public ApplicationThread {
  public:
    RetryCountingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& service_name)
        : ApplicationThread(token, logger, reactor, "RetryCountingThread", ThreadID{1}, make_queue_config(), make_allocator_config("RetryTestPool"),
                            ApplicationThreadConfiguration{})
        , service_name_(service_name) {}

    std::atomic<int> failed_count{0};

  protected:
    void on_initial_event() override {
        connect_to_service(service_name_);
    }
    void on_connection_established(ConnectionID) override {}
    void on_connection_failed(const std::string&) override {
        failed_count.fetch_add(1, std::memory_order_release);
        // deliberately do NOT shut down so the reactor exercises the retry path
    }
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    std::string service_name_;
};

// Test

TEST(OutboundConnectionRetryIntegrationTest, RetryFailedConnectionsReissuesConnectAfterInterval) {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds{10};
    cfg.init_phase_timeout_ = std::chrono::milliseconds{10000};
    cfg.shutdown_timeout_ = std::chrono::milliseconds{500};
    cfg.connect_retry_interval_ = std::chrono::milliseconds{1};
    cfg.connect_timeout = std::chrono::milliseconds{50};

    ServiceRegistry registry;
    registry.add("retry_svc", NetworkEndpointConfiguration{"192.0.2.1", 9999}, // TEST-NET -- non-routable
                 NetworkEndpointConfiguration{});

    LoggerWithSink logger;
    auto reactor = std::make_unique<Reactor>(cfg, registry, logger.logger);
    auto thread = ApplicationThread::create<RetryCountingThread>(logger.logger, *reactor, "retry_svc");
    reactor->register_thread(thread);

    std::thread reactor_thread([&]() { reactor->run(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (thread->failed_count.load(std::memory_order_acquire) < 2) {
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    EXPECT_GE(thread->failed_count.load(std::memory_order_acquire), 2) << "Expected >=2 connection_failed events (initial timeout + retry)";

    reactor->shutdown("test complete");
    if (reactor_thread.joinable()) {
        reactor_thread.join();
    }
}

namespace {

// A port on the loopback interface that nothing is listening on now: bound by the operating system's choice, read back,
// and released.
uint16_t a_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    ::close(fd);
    return ntohs(address.sin_port);
}

ReactorConfiguration a_minute_between_attempts() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds{10};
    cfg.init_phase_timeout_ = std::chrono::milliseconds{10000};
    cfg.shutdown_timeout_ = std::chrono::milliseconds{1000};
    cfg.connect_retry_interval_ = std::chrono::minutes{1};
    cfg.connect_timeout = std::chrono::milliseconds{2000};
    cfg.metrics_configuration.enabled = false;
    cfg.socket_maximum_inactivity_interval_ = std::chrono::minutes{10};
    return cfg;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

// Connects to "peer" when the application is ready, or in its initial event, and calls connect_now("peer") whenever a
// test sets ask_to_connect_now, from its own thread on a one-millisecond timer.
class ConnectingThread : public ApplicationThread {
  public:
    ConnectingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, bool connect_in_initial_event)
        : ApplicationThread(token, logger, reactor, "ConnectingThread", ThreadID{1}, make_queue_config(), make_allocator_config("ConnectingPool"),
                            ApplicationThreadConfiguration{})
        , connect_in_initial_event_(connect_in_initial_event) {}

    std::atomic<int> established{0};
    std::atomic<bool> ask_to_connect_now{false};

  protected:
    void on_initial_event() override {
        if (connect_in_initial_event_) {
            connect_to_service("peer");
        }
    }
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        if (!connect_in_initial_event_) {
            connect_to_service("peer");
        }
    }
    void on_timer_event(TimerID id) override {
        if (id == timer_ && ask_to_connect_now.exchange(false)) {
            connect_now("peer");
        }
    }
    void on_connection_established(ConnectionID) override {
        established.fetch_add(1);
    }
    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

  private:
    bool connect_in_initial_event_;
    TimerID timer_{};
};

// Owns the connections made to a listener, and counts them.
class AcceptingThread : public ApplicationThread {
  public:
    AcceptingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id)
        : ApplicationThread(token, logger, reactor, "AcceptingThread" + std::to_string(id.get_value()), id, make_queue_config(),
                            make_allocator_config("AcceptingPool" + std::to_string(id.get_value())), ApplicationThreadConfiguration{}) {}

    std::atomic<int> accepted{0};

  protected:
    void on_connection_established(ConnectionID) override {
        accepted.fetch_add(1);
    }
    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// Runs a reactor on a thread of its own, and stops it and joins the thread when it goes.
class RunningReactor {
  public:
    ~RunningReactor() {
        reactor_.shutdown("test complete");
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    explicit RunningReactor(Reactor& reactor) : reactor_(reactor), thread_([this]() { reactor_.run(); }) {}
    RunningReactor(const RunningReactor&) = delete;
    RunningReactor& operator=(const RunningReactor&) = delete;

  private:
    Reactor& reactor_;
    std::thread thread_;
};

} // namespaces

// The peer is not listening when the first attempt is made, so the next is a minute away. The peer then starts
// listening. Without connect_now nothing connects within a second; with it, the connection is made at once.
TEST(OutboundConnectionRetryIntegrationTest, ConnectNowMakesAWaitingAttemptAtOnce) {
    const uint16_t port = a_free_port();
    ServiceRegistry registry;
    registry.add("peer", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
    LoggerWithSink logger;

    Reactor connecting_reactor(a_minute_between_attempts(), registry, logger.logger);
    auto connecting = ApplicationThread::create<ConnectingThread>(logger.logger, connecting_reactor, false);
    connecting_reactor.register_thread(connecting);
    RunningReactor running_connector(connecting_reactor);
    ASSERT_TRUE(wait_for([&]() { return connecting_reactor.is_initialized(); }, std::chrono::seconds{5}));
    // Long enough for the first attempt to have been refused and the next scheduled.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    ASSERT_EQ(connecting->established.load(), 0);

    static const ServiceRegistry empty_registry;
    Reactor listening_reactor(a_minute_between_attempts(), empty_registry, logger.logger);
    auto accepting = ApplicationThread::create<AcceptingThread>(logger.logger, listening_reactor, ThreadID{2});
    listening_reactor.register_thread(accepting);
    listening_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", port}, ThreadID{2});
    RunningReactor running_listener(listening_reactor);
    ASSERT_TRUE(wait_for([&]() { return listening_reactor.is_initialized(); }, std::chrono::seconds{5}));

    EXPECT_FALSE(wait_for([&]() { return connecting->established.load() > 0; }, std::chrono::seconds{1}))
        << "connected without being asked, so this test could not tell connect_now from a retry";

    const auto asked = std::chrono::steady_clock::now();
    connecting->ask_to_connect_now.store(true);
    ASSERT_TRUE(wait_for([&]() { return connecting->established.load() == 1; }, std::chrono::seconds{1})) << "connect_now did not make the waiting attempt";
    EXPECT_LT(std::chrono::steady_clock::now() - asked, std::chrono::milliseconds{500});
    EXPECT_TRUE(wait_for([&]() { return accepting->accepted.load() == 1; }, std::chrono::seconds{1}));
}

// A service that is connected has no attempt waiting, and connect_now leaves it alone: no second connection is made.
TEST(OutboundConnectionRetryIntegrationTest, ConnectNowLeavesAConnectedServiceAlone) {
    static const ServiceRegistry empty_registry;
    LoggerWithSink logger;
    Reactor listening_reactor(a_minute_between_attempts(), empty_registry, logger.logger);
    auto accepting = ApplicationThread::create<AcceptingThread>(logger.logger, listening_reactor, ThreadID{2});
    listening_reactor.register_thread(accepting);
    listening_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    RunningReactor running_listener(listening_reactor);
    ASSERT_TRUE(wait_for([&]() { return listening_reactor.is_initialized(); }, std::chrono::seconds{5}));

    ServiceRegistry registry;
    registry.add("peer", NetworkEndpointConfiguration{"127.0.0.1", listening_reactor.get_inbound_listener_port(0)}, NetworkEndpointConfiguration{});
    Reactor connecting_reactor(a_minute_between_attempts(), registry, logger.logger);
    auto connecting = ApplicationThread::create<ConnectingThread>(logger.logger, connecting_reactor, false);
    connecting_reactor.register_thread(connecting);
    RunningReactor running_connector(connecting_reactor);
    ASSERT_TRUE(wait_for([&]() { return connecting->established.load() == 1; }, std::chrono::seconds{5}));

    connecting->ask_to_connect_now.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    EXPECT_FALSE(connecting->ask_to_connect_now.load()) << "the thread never asked";
    EXPECT_EQ(connecting->established.load(), 1);
    EXPECT_EQ(accepting->accepted.load(), 1);
}

// A matching engine opens its connection for reports to the sequencer in its thread's initial event, and the sequencer
// takes that connection's arrival to mean the engine is listening for orders. That holds because a connection asked
// for in the initial event is made only once the reactor's own listeners are bound. Here a thread connects, in its
// initial event, to a listener of its own reactor: if the attempt were made before the listener was bound, it would be
// refused, and the next attempt would be a minute away.
TEST(OutboundConnectionRetryIntegrationTest, AConnectionAskedForInTheInitialEventIsMadeOnceThisReactorsListenersAreBound) {
    const uint16_t port = a_free_port();
    ServiceRegistry registry;
    registry.add("peer", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
    LoggerWithSink logger;
    Reactor reactor(a_minute_between_attempts(), registry, logger.logger);
    auto connecting = ApplicationThread::create<ConnectingThread>(logger.logger, reactor, true);
    reactor.register_thread(connecting);
    auto accepting = ApplicationThread::create<AcceptingThread>(logger.logger, reactor, ThreadID{2});
    reactor.register_thread(accepting);
    reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", port}, ThreadID{2});
    RunningReactor running(reactor);

    EXPECT_TRUE(wait_for([&]() { return connecting->established.load() == 1; }, std::chrono::seconds{2}))
        << "the connection asked for in the initial event was not made at once, so it was attempted before the listener was bound";
    EXPECT_TRUE(wait_for([&]() { return accepting->accepted.load() == 1; }, std::chrono::seconds{1}));
}

} // namespaces
