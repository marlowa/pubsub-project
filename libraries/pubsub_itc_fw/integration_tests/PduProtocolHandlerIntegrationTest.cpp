// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file PduProtocolHandlerIntegrationTest.cpp
 * @brief Integration tests for PduProtocolHandler over loopback TCP.
 *
 * Tests:
 *
 *   LargeQueryNameForcesPartialsend
 *     The connector sends a 2 MB DataQuery. The listener reactor has a 4 MB
 *     inbound_slab_size to accommodate the payload. The listener replies with
 *     a small DataResponse and the connector disconnects cleanly.
 *
 *   LargeResponseForcesPartialSend
 *     The listener replies with a 2 MB DataResponse. The listener reactor has
 *     socket_send_buffer_size set to 16384 bytes, which forces the outbound
 *     write to block and exercises PduProtocolHandler::continue_send(). The
 *     connector verifies the response length.
 *
 *   LargeQueryNameForcesOutboundWriteReady
 *     The connector sends a 2 MB DataQuery with socket_send_buffer_size set
 *     to 16384 bytes on the connector reactor. This forces the outbound write
 *     to block and exercises OutboundConnectionManager::on_write_ready(). The
 *     listener verifies the query arrived intact and replies with a small
 *     DataResponse.
 *
 *   ListenerClosesConnection
 *     The connector connects and sends a small DataQuery. The listener closes
 *     the connection immediately without replying. This exercises the
 *     peer-closed path in OutboundConnectionManager::on_data_ready and the
 *     disconnect handler lambda constructed in on_connect_ready.
 *
 *   DoubleSendThenTeardown
 *     The connector sends two 2 MB DataQuerys back-to-back in
 *     on_connection_established with a small send buffer, guaranteeing the
 *     reactor stashes the second into pending_send_ while the first is still
 *     blocked. The listener closes immediately, triggering teardown_connection
 *     while both conn.has_pending_send() and pending_send_.has_value() are
 *     true. This covers the pending-send cleanup branches in
 *     teardown_connection and the drain_pending_send body.
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

static constexpr int16_t PDU_ID_DATA_QUERY    = 300;
static constexpr int16_t PDU_ID_DATA_RESPONSE = 301;

// 2 MB string — large enough to exceed any loopback socket send buffer,
// guaranteeing at least one partial send regardless of kernel defaults.
static constexpr size_t large_string_bytes = 2 * 1024 * 1024;

// Outbound slab must hold the full encoded PDU in a single chunk:
// large_string_bytes + 4-byte DSL length prefix + 16-byte PduHeader + margin.
static constexpr size_t outbound_slab_size = 4 * 1024 * 1024;

// Inbound decode arena must accommodate the large string field on the
// receiving side.
static constexpr size_t inbound_decode_arena_size = 3 * 1024 * 1024;

// SO_SNDBUF value applied to the sending socket to guarantee blocking.
// The kernel doubles this value internally, giving an effective buffer of
// 2 * small_send_buffer_size bytes — well below the 2 MB payload.
static constexpr int small_send_buffer_size = 16384;

// Size of the large DataResponse results string used in LargeResponseForcesPartialSend.
static constexpr size_t large_response_string_bytes = 2 * 1024 * 1024;

// Outbound slab on the listener for the large-response test.
static constexpr size_t listener_outbound_slab_size = 4 * 1024 * 1024;

// ============================================================
// Helpers
// ============================================================

static ApplicationThreadConfiguration make_connector_thread_config() {
    ApplicationThreadConfiguration cfg{};
    cfg.outbound_slab_size        = outbound_slab_size;
    cfg.inbound_decode_arena_size = inbound_decode_arena_size;
    return cfg;
}

static ApplicationThreadConfiguration make_listener_thread_config() {
    ApplicationThreadConfiguration cfg{};
    cfg.inbound_decode_arena_size = inbound_decode_arena_size;
    return cfg;
}

// ============================================================
// Connector thread: sends a 2 MB DataQuery, decodes the response,
// then disconnects.
// ============================================================
class PduProtocolHandlerConnectorThread : public ApplicationThread {
public:
    PduProtocolHandlerConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "PduProtocolHandlerConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("PduProtocolHandlerConnectorPool"),
                            make_connector_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_sent{false};
    std::atomic<bool> response_received{false};
    std::atomic<bool> connection_lost{false};

    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        large_query_name_.assign(large_string_bytes, 'x');

        DataQuery query{};
        query.request_id = 99;
        query.query_name = std::string_view(large_query_name_);
        query.has_limit  = false;
        query.limit      = 0;

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

        DataResponseView response{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(response, payload, size, consumed, arena, arena_needed)) {
            response_received.store(true, std::memory_order_release);
        }

        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    std::string large_query_name_;
};

// ============================================================
// Listener thread: decodes the large DataQuery, verifies its content,
// and replies with a small DataResponse.
// ============================================================
class PduProtocolHandlerListenerThread : public ApplicationThread {
public:
    PduProtocolHandlerListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "PduProtocolHandlerListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("PduProtocolHandlerListenerPool"),
                            make_listener_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_received{false};
    std::atomic<bool> response_sent{false};
    std::atomic<bool> connection_lost{false};

    size_t  received_query_name_length{0};
    bool    received_query_name_all_x{false};
    int64_t received_request_id{0};

    ConnectionID conn_id{};

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

        DataQueryView query{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(query, payload, size, consumed, arena, arena_needed)) {
            received_request_id        = query.request_id;
            received_query_name_length = query.query_name.size();

            bool all_x = true;
            for (size_t i = 0; i < query.query_name.size(); ++i) {
                if (query.query_name[i] != 'x') {
                    all_x = false;
                    break;
                }
            }
            received_query_name_all_x = all_x;
            query_received.store(true, std::memory_order_release);

            std::array<std::string_view, 1> results_data = {"ok"};
            DataResponse response{};
            response.request_id   = query.request_id;
            response.status_code  = 0;
            response.results.data = results_data.data();
            response.results.size = results_data.size();

            send_pdu(conn_id, PDU_ID_DATA_RESPONSE, response);
            response_sent.store(true, std::memory_order_release);
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Listener thread: sends a 2 MB DataResponse to force continue_send()
// on the listener's outbound socket.
// ============================================================
class LargeResponseListenerThread : public ApplicationThread {
public:
    LargeResponseListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "LargeResponseListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("LargeResponseListenerPool"),
                            make_large_response_listener_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> response_sent{false};
    std::atomic<bool> connection_lost{false};

    ConnectionID conn_id{};

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

        DataQueryView query{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (!decode(query, payload, size, consumed, arena, arena_needed)) {
            return;
        }

        large_result_.assign(large_response_string_bytes, 'y');
        std::array<std::string_view, 1> results_data = {std::string_view(large_result_)};

        DataResponse response{};
        response.request_id   = query.request_id;
        response.status_code  = 0;
        response.results.data = results_data.data();
        response.results.size = results_data.size();

        send_pdu(conn_id, PDU_ID_DATA_RESPONSE, response);
        response_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    static ApplicationThreadConfiguration make_large_response_listener_thread_config() {
        ApplicationThreadConfiguration cfg{};
        cfg.outbound_slab_size        = listener_outbound_slab_size;
        cfg.inbound_decode_arena_size = inbound_decode_arena_size;
        return cfg;
    }

    std::string large_result_;
};

// ============================================================
// Connector thread: sends a small DataQuery, receives a large DataResponse,
// then disconnects.
// ============================================================
class SmallQueryConnectorThread : public ApplicationThread {
public:
    SmallQueryConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "SmallQueryConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("SmallQueryConnectorPool"),
                            make_small_query_connector_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_sent{false};
    std::atomic<bool> response_received{false};
    std::atomic<bool> connection_lost{false};

    size_t received_result_length{0};

    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        DataQuery query{};
        query.request_id = 77;
        query.query_name = std::string_view("small_query");
        query.has_limit  = false;
        query.limit      = 0;

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

        DataResponseView response{};
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed     = 0;
        size_t arena_needed = 0;
        if (decode(response, payload, size, consumed, arena, arena_needed)) {
            if (response.results.size > 0) {
                received_result_length = response.results.data[0].size();
            }
            response_received.store(true, std::memory_order_release);
        }

        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    static ApplicationThreadConfiguration make_small_query_connector_thread_config() {
        ApplicationThreadConfiguration cfg{};
        cfg.inbound_decode_arena_size = inbound_decode_arena_size;
        return cfg;
    }
};

// ============================================================
// Test fixture
// ============================================================
class PduProtocolHandlerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 10000) {
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

    // Connector reactor with default OS send buffer.
    static ReactorConfiguration make_connector_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        // Must accommodate the 2 MB DataResponse payload received in
        // LargeResponseForcesPartialSend.
        cfg.inbound_slab_size          = outbound_slab_size;
        return cfg;
    }

    // Listener reactor with a large inbound slab to accommodate the 2 MB DataQuery.
    static ReactorConfiguration make_listener_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        cfg.inbound_slab_size          = outbound_slab_size;
        return cfg;
    }

    // Listener reactor with a small send buffer — forces PduProtocolHandler::continue_send()
    // when the listener sends a 2 MB DataResponse.
    static ReactorConfiguration make_small_sndbuf_listener_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        cfg.inbound_slab_size          = outbound_slab_size;
        cfg.socket_send_buffer_size    = small_send_buffer_size;
        return cfg;
    }

    // Connector reactor with a small send buffer — forces
    // OutboundConnectionManager::on_write_ready() when the connector sends a 2 MB DataQuery.
    static ReactorConfiguration make_small_sndbuf_connector_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(2000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);
        cfg.inbound_slab_size          = outbound_slab_size;
        cfg.socket_send_buffer_size    = small_send_buffer_size;
        return cfg;
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: connector sends a 2 MB DataQuery; listener reassembles it
// and verifies content.
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, LargeQueryNameForcesPartialsend) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<PduProtocolHandlerListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_connector_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = ApplicationThread::create<PduProtocolHandlerConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->query_sent.load(std::memory_order_acquire);
    })) << "Connector: large DataQuery not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->query_received.load(std::memory_order_acquire);
    }, 15000)) << "Listener: large DataQuery not received within timeout";

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

    EXPECT_EQ(listener_thread->received_request_id, 99);
    EXPECT_EQ(listener_thread->received_query_name_length, large_string_bytes);
    EXPECT_TRUE(listener_thread->received_query_name_all_x)
        << "Received query_name contained unexpected characters";

    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

// ============================================================
// Test: listener sends a 2 MB DataResponse with a small send buffer,
// exercising PduProtocolHandler::continue_send().
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, LargeResponseForcesPartialSend) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_small_sndbuf_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<LargeResponseListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_connector_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = ApplicationThread::create<SmallQueryConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

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
        return listener_thread->response_sent.load(std::memory_order_acquire);
    })) << "Listener: large DataResponse not sent";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->response_received.load(std::memory_order_acquire);
    }, 15000)) << "Connector: large DataResponse not received within timeout";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after disconnect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer disconnect";

    EXPECT_EQ(connector_thread->received_result_length, large_response_string_bytes);

    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

// ============================================================
// Test: connector sends a 2 MB DataQuery with a small send buffer,
// exercising OutboundConnectionManager::on_write_ready().
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, LargeQueryNameForcesOutboundWriteReady) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<PduProtocolHandlerListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // The connector's socket gets a small send buffer, forcing its 2 MB write
    // to block and causing the reactor to call on_write_ready() repeatedly.
    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_small_sndbuf_connector_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = ApplicationThread::create<PduProtocolHandlerConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->query_sent.load(std::memory_order_acquire);
    })) << "Connector: large DataQuery not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->query_received.load(std::memory_order_acquire);
    }, 15000)) << "Listener: large DataQuery not received within timeout";

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

    EXPECT_EQ(listener_thread->received_request_id, 99);
    EXPECT_EQ(listener_thread->received_query_name_length, large_string_bytes);
    EXPECT_TRUE(listener_thread->received_query_name_all_x)
        << "Received query_name contained unexpected characters";

    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

// ============================================================
// Listener thread: disconnects immediately on receiving a query
// without sending any response. Used by ListenerClosesConnection.
// ============================================================
class DisconnectingListenerThread : public ApplicationThread {
public:
    DisconnectingListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "DisconnectingListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("DisconnectingListenerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_lost{false};

    ConnectionID conn_id{};

protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("disconnected");
    }

    void on_framework_pdu_message([[maybe_unused]] const EventMessage& msg) override {
        // Deliberately close the connection without replying, exercising the
        // outbound disconnect handler lambda in OutboundConnectionManager::on_connect_ready
        // and the peer-closed path in OutboundConnectionManager::on_data_ready.
        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(cmd);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test: listener closes the connection after receiving a query,
// exercising OutboundConnectionManager::on_data_ready peer-closed path
// and the disconnect handler lambda in on_connect_ready.
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, ListenerClosesConnection) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<DisconnectingListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_connector_reactor_config(), connector_registry, logger_->logger);

    // Use SmallQueryConnectorThread — it sends a small DataQuery on connect
    // and calls shutdown on connection lost, which is what we expect here
    // since the listener will close without replying.
    auto connector_thread = ApplicationThread::create<SmallQueryConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->query_sent.load(std::memory_order_acquire);
    })) << "Connector: DataQuery not sent";

    // The listener closes the connection after receiving the query.
    // The connector should receive ConnectionLost (peer closed, no response).
    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after listener closed connection";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received";

    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

// ============================================================
// Connector thread: sends two large PDUs back-to-back in
// on_connection_established, before the reactor has processed either.
// This guarantees:
//   1. The reactor processes the first SendPdu, finds has_pending_data()
//      true, and calls set_pending_send — conn.has_pending_send() is true.
//   2. The reactor processes the second SendPdu while the first is still
//      blocked, and stashes it into pending_send_ — pending_send_.has_value()
//      is true.
// Used by DoubleSendThenTeardown.
// ============================================================
class DoubleSendConnectorThread : public ApplicationThread {
public:
    DoubleSendConnectorThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "DoubleSendConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("DoubleSendConnectorPool"),
                            make_double_send_connector_thread_config()) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> both_sent{false};
    std::atomic<bool> connection_lost{false};

    ConnectionID conn_id{};

protected:
    void on_initial_event() override {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        // Both PDUs are enqueued before the reactor processes either.
        // The slab holds two large frames so both allocations succeed here.
        first_payload_.assign(large_string_bytes, 'a');
        second_payload_.assign(large_string_bytes, 'b');

        DataQuery first{};
        first.request_id = 1;
        first.query_name = std::string_view(first_payload_);
        first.has_limit  = false;
        first.limit      = 0;
        send_pdu(id, PDU_ID_DATA_QUERY, first);

        DataQuery second{};
        second.request_id = 2;
        second.query_name = std::string_view(second_payload_);
        second.has_limit  = false;
        second.limit      = 0;
        send_pdu(id, PDU_ID_DATA_QUERY, second);

        both_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_framework_pdu_message([[maybe_unused]] const EventMessage& msg) override {}

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    static ApplicationThreadConfiguration make_double_send_connector_thread_config() {
        ApplicationThreadConfiguration cfg{};
        // Must fit two 2 MB frames: 2 * outbound_slab_size with margin.
        // Each send_pdu call allocates from the same ExpandableSlabAllocator,
        // which chains a new slab when the first is full, so outbound_slab_size
        // per slab is sufficient — the allocator expands automatically.
        cfg.outbound_slab_size        = outbound_slab_size;
        cfg.inbound_decode_arena_size = inbound_decode_arena_size;
        return cfg;
    }

    std::string first_payload_;
    std::string second_payload_;
};

// ============================================================
// Test: connector sends two large PDUs back-to-back with a small send
// buffer, then the listener closes immediately on receiving the first.
// This covers:
//   - OutboundConnectionManager: conn.has_pending_send() branch in
//     teardown_connection (lines 451-452)
//   - OutboundConnectionManager: pending_send_.has_value() branch in
//     teardown_connection (lines 457-459)
//   - OutboundConnectionManager: drain_pending_send body (lines 364-375)
// ============================================================
TEST_F(PduProtocolHandlerIntegrationTest, DoubleSendThenTeardown) {
    // Listener closes the connection immediately on receiving any PDU.
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_listener_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<DisconnectingListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return listener_reactor->is_initialized(); }))
        << "Listener reactor did not initialise within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // Small send buffer on the connector forces the first PDU to block,
    // guaranteeing set_pending_send is called before the second SendPdu
    // command is processed — which then stashes it into pending_send_.
    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfiguration{"127.0.0.1", listen_port},
        NetworkEndpointConfiguration{});

    auto connector_reactor = std::make_unique<Reactor>(
        make_small_sndbuf_connector_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = ApplicationThread::create<DoubleSendConnectorThread>(
        logger_->logger, *connector_reactor);
    connector_reactor->register_thread(connector_thread);

    std::thread connector_reactor_thread([&]() { connector_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_established.load(std::memory_order_acquire);
    })) << "Connector: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->both_sent.load(std::memory_order_acquire);
    })) << "Connector: both PDUs not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // The listener closes after receiving the first PDU (or any fragment of it).
    // The connector's teardown_connection fires while:
    //   conn.has_pending_send() is true  (first PDU still blocked), and
    //   pending_send_.has_value() is true (second PDU stashed).
    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    }, 15000)) << "Connector: ConnectionLost not received after listener teardown";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received";

    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

} // namespace pubsub_itc_fw::tests
