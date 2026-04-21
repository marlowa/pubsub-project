// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file VariableLengthPduTest.cpp
 * @brief Integration test: variable-length PDU exchange over loopback TCP.
 *
 * Exercises the variable-length encoding and decoding paths end-to-end:
 *   - DataQuery carries a string field, an optional i32, and a fixed i64.
 *   - DataResponse carries a fixed i64, a fixed i32, and a list<string>.
 *
 * These message types require arena allocation during decode, unlike the
 * fixed-size leader-follower PDUs. This test verifies that the pre-allocated
 * decode arena buffer on ApplicationThread handles both cases correctly.
 *
 * Test structure mirrors StatusQueryResponseTest:
 *   - One reactor (listener) registers an inbound listener on a
 *     dynamically-assigned loopback port (127.0.0.1:0).
 *   - A second reactor (connector) connects to that port.
 *   - The connector sends a DataQuery on ConnectionEstablished.
 *   - The listener decodes it, builds a DataResponse with multiple
 *     result strings, and sends it back.
 *   - The connector decodes the DataResponse and verifies all fields.
 *   - The connector disconnects cleanly.
 */

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
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <variable_length_test_protocol.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// PDU IDs matching the variable-length test protocol DSL
// ============================================================
static constexpr int16_t PDU_ID_DATA_QUERY    = 300;
static constexpr int16_t PDU_ID_DATA_RESPONSE = 301;

// ============================================================
// Helpers
// ============================================================

// ============================================================
// Connector-side ApplicationThread.
// Sends DataQuery on ConnectionEstablished.
// Decodes DataResponse and verifies field values.
// Disconnects cleanly.
// ============================================================
class ConnectorThread : public ApplicationThread {
public:
    ConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "ConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("ConnectorPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_sent{false};
    std::atomic<bool> response_received{false};
    std::atomic<bool> connection_lost{false};

    DataResponseView received_response{};
    std::vector<std::string> received_results;
    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        DataQuery query{};
        query.request_id  = 42;
        query.query_name  = std::string_view("find_everything");
        query.has_limit   = true;
        query.limit       = 10;

        send_pdu(id, PDU_ID_DATA_QUERY, query);
        query_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t   size    = static_cast<size_t>(msg.payload_size());

        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(received_response, payload, size, consumed, arena, arena_needed)) {
            // Copy the list<string> results out of the arena into owned strings.
            // The arena buffer is reused on the next PDU call so we must copy now.
            received_results.clear();
            for (size_t i = 0; i < received_response.results.size; ++i) {
                received_results.emplace_back(received_response.results.data[i]);
            }
            response_received.store(true, std::memory_order_release);
        }

        // Disconnect now that we have the response.
        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Listener-side ApplicationThread.
// Decodes DataQuery, replies with DataResponse containing
// a list of result strings.
// ============================================================
class ListenerThread : public ApplicationThread {
public:
    ListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "ListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("ListenerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_received{false};
    std::atomic<bool> response_sent{false};
    std::atomic<bool> connection_lost{false};

    DataQueryView received_query{};
    std::string   received_query_name;
    bool          received_has_limit{false};
    int32_t       received_limit{0};
    ConnectionID  conn_id{};

protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("peer disconnected");
    }

    void on_framework_pdu_message(const EventMessage& msg) override {
        const uint8_t* payload = msg.payload();
        const size_t   size    = static_cast<size_t>(msg.payload_size());

        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(received_query, payload, size, consumed, arena, arena_needed)) {
            // Copy string fields out of the arena into owned storage.
            received_query_name  = std::string(received_query.query_name);
            received_has_limit   = received_query.has_limit;
            received_limit       = received_query.limit;
            query_received.store(true, std::memory_order_release);

            // Build a response with three result strings.
            // ListView requires pointing into a contiguous array; we use a
            // local array of string_views backed by string literals, which
            // are valid for the lifetime of this call. The encoder reads
            // them before returning.
            std::array<std::string_view, 3> results_data = {
                "alpha", "beta", "gamma"
            };

            DataResponse response{};
            response.request_id  = received_query.request_id;
            response.status_code = 0;
            response.results.data = results_data.data();
            response.results.size = results_data.size();

            send_pdu(conn_id, PDU_ID_DATA_RESPONSE, response);
            response_sent.store(true, std::memory_order_release);
        }

    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class VariableLengthPduTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>("vl_integ_logger", "vl_integ_sink");
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

    static ReactorConfiguration make_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        return cfg;
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: full DataQuery / DataResponse round-trip over loopback
// ============================================================
TEST_F(VariableLengthPduTest, DataQueryResponseRoundTrip) {
    // --- Listener side ---
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<ListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // --- Connector side ---
    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = ApplicationThread::create<ConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    // --- Wait for each step of the exchange ---

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->query_sent.load(std::memory_order_acquire);
    })) << "Connector: DataQuery not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->query_received.load(std::memory_order_acquire);
    })) << "Listener: DataQuery not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->response_sent.load(std::memory_order_acquire);
    })) << "Listener: DataResponse not sent";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->response_received.load(std::memory_order_acquire);
    })) << "Connector: DataResponse not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after disconnect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer disconnect";

    // --- Verify DataQuery fields on the listener ---
    EXPECT_EQ(listener_thread->received_query.request_id, 42);
    EXPECT_EQ(listener_thread->received_query_name, "find_everything");
    EXPECT_TRUE(listener_thread->received_has_limit);
    EXPECT_EQ(listener_thread->received_limit, 10);

    // --- Verify DataResponse fields on the connector ---
    EXPECT_EQ(connector_thread->received_response.request_id, 42);
    EXPECT_EQ(connector_thread->received_response.status_code, 0);
    ASSERT_EQ(connector_thread->received_results.size(), 3u);
    EXPECT_EQ(connector_thread->received_results[0], "alpha");
    EXPECT_EQ(connector_thread->received_results[1], "beta");
    EXPECT_EQ(connector_thread->received_results[2], "gamma");

    // --- Shutdown both reactors ---
    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) {
        connector_reactor_thread.join();
    }
    if (listener_reactor_thread.joinable()) {
        listener_reactor_thread.join();
    }
}

} // namespace pubsub_itc_fw::tests
