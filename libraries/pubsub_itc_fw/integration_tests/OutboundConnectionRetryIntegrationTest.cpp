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
 */

#include <atomic>
#include <chrono>
#include <memory>
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
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

using pubsub_itc_fw::tests::make_allocator_config;
using pubsub_itc_fw::tests::make_queue_config;

namespace pubsub_itc_fw {

// ============================================================
// Application thread that counts connection failures without
// shutting down the reactor, so the retry path can fire.
// ============================================================
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

// ============================================================
// Test
// ============================================================

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

} // namespaces
