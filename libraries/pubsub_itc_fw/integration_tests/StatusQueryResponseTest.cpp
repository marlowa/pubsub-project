// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file StatusQueryResponseTest.cpp
 * @brief Integration test: StatusQuery / StatusResponse exchange over loopback TCP.
 *
 * Exercises the full framework socket path end-to-end:
 *   - One reactor (the listener side) registers an inbound listener on a
 *     dynamically-assigned loopback port (127.0.0.1:0).
 *   - A second reactor (the connector side) connects to that port.
 *   - The connector sends a StatusQuery PDU on ConnectionEstablished.
 *   - The listener receives it, decodes it, and sends back a StatusResponse.
 *   - The connector receives and decodes the StatusResponse.
 *   - The connector disconnects cleanly.
 *   - Payload field values are verified end-to-end.
 *
 * Both reactors run on separate std::threads simultaneously.
 * The OS-assigned port is read back via get_first_inbound_listener_port()
 * after the listener reactor has initialized.
 */

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

#include <leader_follower.hpp>

namespace pubsub_itc_fw {

// ============================================================
// PDU IDs matching the leader-follower protocol DSL
// ============================================================
static constexpr int16_t PDU_ID_STATUS_QUERY    = 100;
static constexpr int16_t PDU_ID_STATUS_RESPONSE = 101;

// ============================================================
// Helpers
// ============================================================

static QueueConfig make_queue_config()
{
    QueueConfig cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static AllocatorConfig make_allocator_config(const std::string& name)
{
    AllocatorConfig cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

// Build a complete PDU frame (PduHeader + DSL-encoded payload) in the
// outbound slab allocator. Returns {slab_id, frame_ptr, total_frame_bytes}.
// Returns {-1, nullptr, 0} on allocation failure.
template<typename MsgT>
static std::tuple<int, void*, uint32_t>
build_pdu_frame(Reactor& reactor, int16_t pdu_id, const MsgT& msg)
{
    const size_t payload_size = fixed_encoded_size(msg);
    const size_t frame_size   = sizeof(PduHeader) + payload_size;

    auto [slab_id, chunk] = reactor.outbound_slab_allocator().allocate(frame_size);
    if (chunk == nullptr) {
        return {-1, nullptr, 0};
    }

    PduHeader* hdr  = reinterpret_cast<PduHeader*>(chunk);
    hdr->byte_count = htonl(static_cast<uint32_t>(payload_size));
    hdr->pdu_id     = htons(static_cast<uint16_t>(pdu_id));
    hdr->version    = 1;
    hdr->filler_a   = 0;
    hdr->canary     = htonl(pdu_canary_value);
    hdr->filler_b   = 0;

    encode_fast(msg, static_cast<uint8_t*>(chunk) + sizeof(PduHeader));

    return {slab_id, chunk, static_cast<uint32_t>(frame_size)};
}

static void enqueue_send_pdu(Reactor& reactor, ConnectionID conn_id,
                              int slab_id, void* chunk, uint32_t total_bytes)
{
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::SendPdu);
    cmd.connection_id_  = conn_id;
    cmd.slab_id_        = slab_id;
    cmd.pdu_chunk_ptr_  = chunk;
    cmd.pdu_byte_count_ = total_bytes - static_cast<uint32_t>(sizeof(PduHeader));
    reactor.enqueue_control_command(cmd);
}

static void enqueue_disconnect(Reactor& reactor, ConnectionID conn_id)
{
    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = conn_id;
    reactor.enqueue_control_command(cmd);
}

// ============================================================
// Connector-side ApplicationThread.
// Sends Connect on initial event.
// Sends StatusQuery on ConnectionEstablished.
// Decodes StatusResponse and disconnects.
// ============================================================
class ConnectorThread : public ApplicationThread {
public:
    ConnectorThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "ConnectorThread", ThreadID{1},
                            make_queue_config(), make_allocator_config("ConnectorPool"))
    {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_sent{false};
    std::atomic<bool> response_received{false};
    std::atomic<bool> connection_lost{false};

    StatusResponseView received_response{};
    ConnectionID       conn_id{};

protected:
    void on_initial_event() override
    {
        connect_to_service("listener");
    }

    void on_connection_established(ConnectionID id) override
    {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);

        StatusQuery query{};
        query.instance_id = 42;
        query.epoch       = 7;

        auto [slab_id, chunk, total_bytes] =
            build_pdu_frame(get_reactor(), PDU_ID_STATUS_QUERY, query);
        if (chunk == nullptr) {
            return;
        }

        enqueue_send_pdu(get_reactor(), id, slab_id, chunk, total_bytes);
        query_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string& reason) override
    {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(ConnectionID, const std::string&) override
    {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& msg) override
    {
        const uint8_t* payload     = msg.payload();
        const size_t   size        = static_cast<size_t>(msg.payload_size());
        size_t         consumed    = 0;
        size_t         arena_bytes = 0;
        BumpAllocator  dummy(nullptr, 0);

        if (decode(received_response, payload, size, consumed, dummy, arena_bytes)) {
            response_received.store(true, std::memory_order_release);
        }

        get_reactor().inbound_slab_allocator().deallocate(
            msg.slab_id(), const_cast<uint8_t*>(payload));

        // Disconnect now that we have the response.
        enqueue_disconnect(get_reactor(), conn_id);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Listener-side ApplicationThread.
// Decodes StatusQuery, replies with StatusResponse.
// Shuts down when connection is lost.
// ============================================================
class ListenerThread : public ApplicationThread {
public:
    ListenerThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "ListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("ListenerPool"))
    {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> query_received{false};
    std::atomic<bool> response_sent{false};
    std::atomic<bool> connection_lost{false};

    StatusQueryView received_query{};
    ConnectionID    conn_id{};

protected:
    void on_connection_established(ConnectionID id) override
    {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override
    {
        connection_lost.store(true, std::memory_order_release);
        shutdown("peer disconnected");
    }

    void on_framework_pdu_message(const EventMessage& msg) override
    {
        const uint8_t* payload     = msg.payload();
        const size_t   size        = static_cast<size_t>(msg.payload_size());
        size_t         consumed    = 0;
        size_t         arena_bytes = 0;
        BumpAllocator  dummy(nullptr, 0);

        if (decode(received_query, payload, size, consumed, dummy, arena_bytes)) {
            query_received.store(true, std::memory_order_release);

            StatusResponse response{};
            response.self_instance_id = 99;
            response.peer_instance_id = received_query.instance_id;
            response.epoch            = received_query.epoch;

            auto [slab_id, chunk, total_bytes] =
                build_pdu_frame(get_reactor(), PDU_ID_STATUS_RESPONSE, response);
            if (chunk != nullptr) {
                enqueue_send_pdu(get_reactor(), conn_id, slab_id, chunk, total_bytes);
                response_sent.store(true, std::memory_order_release);
            }
        }

        get_reactor().inbound_slab_allocator().deallocate(
            msg.slab_id(), const_cast<uint8_t*>(payload));
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class StatusQueryResponseTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        logger_ = std::make_unique<LoggerWithSink>("integ_logger", "integ_sink");
    }

    void TearDown() override
    {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    static ReactorConfiguration make_reactor_config()
    {
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
// Test: full StatusQuery / StatusResponse round-trip over loopback
// ============================================================
TEST_F(StatusQueryResponseTest, StatusQueryResponseRoundTrip)
{
    // --- Listener side ---
    ServiceRegistry listener_registry;  // no outbound connections needed
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    // port=0 asks the OS to assign a free port
    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = std::make_shared<ListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });

    // Wait for the listener reactor to finish initializing — at that point
    // the acceptor socket is bound and the OS-assigned port is readable.
    ASSERT_TRUE(wait_for([&]() {
        return listener_reactor->is_initialized();
    })) << "Listener reactor did not initialize within timeout";

    const uint16_t listen_port = listener_reactor->get_first_inbound_listener_port();
    ASSERT_NE(listen_port, 0u) << "OS did not assign a valid listening port";

    // --- Connector side ---
    ServiceRegistry connector_registry;
    connector_registry.add("listener",
        NetworkEndpointConfig{"127.0.0.1", listen_port},
        NetworkEndpointConfig{});  // no secondary

    auto connector_reactor = std::make_unique<Reactor>(
        make_reactor_config(), connector_registry, logger_->logger);

    auto connector_thread = std::make_shared<ConnectorThread>(
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
    })) << "Connector: StatusQuery not sent";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->query_received.load(std::memory_order_acquire);
    })) << "Listener: StatusQuery not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->response_sent.load(std::memory_order_acquire);
    })) << "Listener: StatusResponse not sent";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->response_received.load(std::memory_order_acquire);
    })) << "Connector: StatusResponse not received";

    EXPECT_TRUE(wait_for([&]() {
        return connector_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Connector: ConnectionLost not received after disconnect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer disconnect";

    // --- Verify payload field values ---
    EXPECT_EQ(listener_thread->received_query.instance_id, 42);
    EXPECT_EQ(listener_thread->received_query.epoch,        7);

    EXPECT_EQ(connector_thread->received_response.self_instance_id, 99);
    EXPECT_EQ(connector_thread->received_response.peer_instance_id, 42);
    EXPECT_EQ(connector_thread->received_response.epoch,             7);

    // --- Shutdown both reactors ---
    listener_reactor->shutdown("test complete");
    connector_reactor->shutdown("test complete");

    if (connector_reactor_thread.joinable()) { connector_reactor_thread.join(); }
    if (listener_reactor_thread.joinable())  { listener_reactor_thread.join(); }
}

} // namespace pubsub_itc_fw
