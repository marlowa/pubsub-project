// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file RawBytesProtocolHandlerTest.cpp
 * @brief Integration tests for RawBytesProtocolHandler and InboundConnectionManager.
 *
 * Test protocol framing used throughout:
 *   [ uint32_t length (big-endian) ][ payload bytes ]
 *
 * This is deliberately distinct from the framework's PduHeader to make clear
 * that RawBytesProtocolHandler places no constraints on byte stream structure.
 *
 * All connector-side sockets are raw POSIX sockets (::socket / ::connect /
 * ::send / ::recv / ::close). This models the realistic FIX gateway scenario
 * where the connecting client has no knowledge of the framework, and avoids
 * any confusion between framework SendPdu/SendRaw command routing and the
 * raw byte stream that RawBytesProtocolHandler is designed to handle.
 *
 * The listener side uses the full reactor + ApplicationThread machinery,
 * including commit_raw_bytes() and send_raw(), which is the production code
 * path that needs coverage.
 *
 * Tests in this file:
 *
 *   RawByteRoundTrip
 *     Happy-path: raw socket sends one framed message, listener replies via
 *     send_raw(), raw socket reads and verifies the reply. Covers the basic
 *     on_data_ready / commit_bytes / send_prebuilt path.
 *
 *   FragmentedDelivery
 *     The raw socket sends the 4-byte length prefix and the payload bytes in
 *     two separate ::send() calls with a short sleep between them to maximise
 *     the chance of separate TCP segments reaching the listener. The listener
 *     must return without acting on the first callback (incomplete message),
 *     then decode correctly when the full message has accumulated.
 *
 *   BurstDelivery
 *     The raw socket sends five framed messages back-to-back before the listener
 *     has had a chance to process any. The listener decodes and commits all
 *     messages, verifying ordering and correct tail advancement.
 *
 *   PeerDisconnect
 *     The raw socket connects and then closes without sending data. The listener
 *     must detect recv==0 and deliver ConnectionLost.
 *
 *   OneConnectionRejection
 *     While the listener already has an established connection, a second raw
 *     socket attempts to connect. InboundConnectionManager must silently close
 *     it and deliver no event. The first connection must remain alive.
 *
 *   IdleTimeout
 *     The listener is configured with a very short idle timeout. The raw socket
 *     connects and sends nothing. After the interval the reactor must tear down
 *     the connection and deliver ConnectionLost.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfig.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Test protocol constants
// ============================================================

static const std::string request_payload  = "HELLO_RAW_SERVER";
static const std::string response_payload = "HELLO_RAW_CLIENT";

static constexpr int64_t     raw_buffer_capacity = 65536;
static constexpr std::size_t length_prefix_size  = sizeof(uint32_t);

// ============================================================
// Raw POSIX socket helpers
// ============================================================

/*
 * Builds a framed message [ uint32_t length (big-endian) ][ payload bytes ]
 * into a std::string and returns it. No framework dependency.
 */
static std::string make_framed(const std::string& payload) {
    const uint32_t length_be = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame(length_prefix_size + payload.size(), '\0');
    std::memcpy(frame.data(), &length_be, length_prefix_size);
    std::memcpy(frame.data() + length_prefix_size, payload.data(), payload.size());
    return frame;
}

/*
 * Sends all bytes in buf on sock_fd, retrying on short writes.
 * Returns true on success.
 */
static bool send_all(int sock_fd, const void* buf, std::size_t size) {
    const auto* ptr = static_cast<const char*>(buf);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t sent = ::send(sock_fd, ptr, remaining, 0);
        if (sent <= 0) {
            return false;
        }
        ptr       += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

/*
 * Receives exactly size bytes from sock_fd into buf, retrying on short reads.
 * Returns true on success.
 */
static bool recv_all(int sock_fd, void* buf, std::size_t size) {
    auto* ptr = static_cast<char*>(buf);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t received = ::recv(sock_fd, ptr, remaining, 0);
        if (received <= 0) {
            return false;
        }
        ptr       += received;
        remaining -= static_cast<std::size_t>(received);
    }
    return true;
}

/*
 * Reads and decodes one framed reply from sock_fd.
 * Returns the decoded payload string, or empty string on error.
 */
static std::string recv_framed(int sock_fd) {
    uint32_t length_be = 0;
    if (!recv_all(sock_fd, &length_be, sizeof(length_be))) {
        return {};
    }
    const uint32_t payload_length = ntohl(length_be);
    std::string payload(payload_length, '\0');
    if (!recv_all(sock_fd, payload.data(), payload_length)) {
        return {};
    }
    return payload;
}

/*
 * Opens and connects a raw TCP socket to 127.0.0.1:port.
 * Returns the fd on success or -1 on failure.
 */
static int connect_raw_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ============================================================
// Test protocol decode helpers (used by listener threads)
// ============================================================

/*
 * Attempts to decode one length-prefixed message from a raw byte view.
 * Returns the payload on success; sets bytes_consumed. Returns empty and
 * bytes_consumed=0 if the message is incomplete.
 */
static std::string try_decode_framed(const uint8_t* data, int available,
                                     int64_t& bytes_consumed) {
    bytes_consumed = 0;
    if (available < static_cast<int>(length_prefix_size)) {
        return {};
    }
    uint32_t length_be = 0;
    std::memcpy(&length_be, data, length_prefix_size);
    const uint32_t payload_length = ntohl(length_be);
    const int64_t total = static_cast<int64_t>(length_prefix_size + payload_length);
    if (available < static_cast<int>(total)) {
        return {};
    }
    std::string payload(reinterpret_cast<const char*>(data + length_prefix_size),
                        payload_length);
    bytes_consumed = total;
    return payload;
}

/*
 * Decodes all complete framed messages from the buffer, appending payloads
 * to results and accumulating total_consumed.
 */
static int decode_all_framed(const uint8_t* data, int available,
                              std::vector<std::string>& results,
                              int64_t& total_consumed) {
    int count          = 0;
    total_consumed     = 0;
    int remaining      = available;
    const uint8_t* ptr = data;

    while (remaining > 0) {
        int64_t consumed = 0;
        std::string payload = try_decode_framed(ptr, remaining, consumed);
        if (consumed == 0) {
            break;
        }
        results.push_back(std::move(payload));
        ptr            += consumed;
        remaining      -= static_cast<int>(consumed);
        total_consumed += consumed;
        ++count;
    }
    return count;
}

// ============================================================
// Application thread helpers
// ============================================================

static QueueConfig make_queue_config() {
    QueueConfig cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static AllocatorConfig make_allocator_config(const std::string& name) {
    AllocatorConfig cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

static ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
    cfg.connect_timeout            = std::chrono::milliseconds(2000);
    return cfg;
}

static ReactorConfiguration make_short_idle_reactor_config() {
    ReactorConfiguration cfg = make_reactor_config();
    cfg.socket_maximum_inactivity_interval_ = std::chrono::milliseconds(300);
    cfg.inactivity_check_interval_          = std::chrono::milliseconds(50);
    return cfg;
}

// ============================================================
// Listener thread: decodes one complete message then replies via send_raw().
// Used by RawByteRoundTrip and FragmentedDelivery.
// ============================================================
class RawListenerThread : public ApplicationThread {
public:
    RawListenerThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "RawListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("RawListenerPool"),
                            ApplicationThreadConfig{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> message_received{false};
    std::atomic<bool> reply_sent{false};
    std::atomic<bool> connection_lost{false};
    std::atomic<int>  callback_count{0};

    std::string received_payload;
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

    void on_raw_socket_message(const EventMessage& message) override {
        callback_count.fetch_add(1, std::memory_order_relaxed);

        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0) {
            return;
        }

        int64_t bytes_consumed = 0;
        std::string payload = try_decode_framed(data, available, bytes_consumed);
        if (bytes_consumed == 0) {
            return; // incomplete -- wait for more data
        }

        received_payload = payload;
        message_received.store(true, std::memory_order_release);

        commit_raw_bytes(conn_id, bytes_consumed);

        const std::string reply = make_framed(response_payload);
        send_raw(conn_id, reply.data(), static_cast<uint32_t>(reply.size()));
        reply_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Listener thread: decodes multiple messages from a burst.
// Used by BurstDelivery.
// ============================================================
class BurstListenerThread : public ApplicationThread {
public:
    BurstListenerThread(QuillLogger& logger, Reactor& reactor, int expected_count)
        : ApplicationThread(logger, reactor, "BurstListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("BurstListenerPool"),
                            ApplicationThreadConfig{})
        , expected_count_(expected_count) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> all_received{false};
    std::atomic<bool> connection_lost{false};

    std::vector<std::string> received_payloads;
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

    void on_raw_socket_message(const EventMessage& message) override {
        const uint8_t* data = message.payload();
        const int available = message.payload_size();

        if (data == nullptr || available <= 0) {
            return;
        }

        int64_t total_consumed = 0;
        decode_all_framed(data, available, received_payloads, total_consumed);

        if (total_consumed > 0) {
            commit_raw_bytes(conn_id, total_consumed);
        }

        if (static_cast<int>(received_payloads.size()) >= expected_count_) {
            all_received.store(true, std::memory_order_release);
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    int expected_count_;
};

// ============================================================
// Listener thread: tracks connection events only, does not consume bytes.
// Used by PeerDisconnect, OneConnectionRejection, and IdleTimeout.
// ============================================================
class PassiveListenerThread : public ApplicationThread {
public:
    PassiveListenerThread(QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(logger, reactor, "PassiveListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("PassiveListenerPool"),
                            ApplicationThreadConfig{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_lost{false};
    std::atomic<int>  connection_established_count{0};

    ConnectionID conn_id{};

protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established_count.fetch_add(1, std::memory_order_relaxed);
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
        shutdown("connection lost");
    }

    // Does not commit bytes -- used by IdleTimeout.
    void on_raw_socket_message([[maybe_unused]] const EventMessage& msg) override {}

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class RawBytesProtocolHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>("raw_integ_logger", "raw_integ_sink");
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

    uint16_t start_listener_reactor(Reactor& reactor) {
        EXPECT_TRUE(wait_for([&]() { return reactor.is_initialized(); }))
            << "Listener reactor did not initialise within timeout";
        const uint16_t port = reactor.get_first_inbound_listener_port();
        EXPECT_NE(port, 0u) << "OS did not assign a valid listening port";
        return port;
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread,
                                  const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable()) {
            reactor_thread.join();
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: happy-path round-trip
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, RawByteRoundTrip) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<RawListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    // Use a raw POSIX socket as the connector.
    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Send one framed message.
    const std::string frame = make_framed(request_payload);
    ASSERT_TRUE(send_all(sock_fd, frame.data(), frame.size()))
        << "Failed to send framed request";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: reply not sent";

    // Read the reply on the raw socket.
    const std::string reply = recv_framed(sock_fd);
    EXPECT_EQ(reply, response_payload);

    EXPECT_EQ(listener_thread->received_payload, request_payload);

    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after socket close";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: fragmented delivery
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, FragmentedDelivery) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<RawListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    // Disable Nagle's algorithm so that the two sends below are not coalesced
    // by the kernel into a single TCP segment, maximising the chance of the
    // listener seeing an incomplete message on the first callback.
    const int one = 1;
    ::setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Send length prefix only.
    const uint32_t length_be = htonl(static_cast<uint32_t>(request_payload.size()));
    ASSERT_TRUE(send_all(sock_fd, &length_be, sizeof(length_be)))
        << "Failed to send length prefix";

    // Brief pause to let the reactor process the first partial delivery.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Send payload bytes.
    ASSERT_TRUE(send_all(sock_fd, request_payload.data(), request_payload.size()))
        << "Failed to send payload";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->message_received.load(std::memory_order_acquire);
    })) << "Listener: complete message not received after fragmented send";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: reply not sent";

    const std::string reply = recv_framed(sock_fd);
    EXPECT_EQ(reply, response_payload);
    EXPECT_EQ(listener_thread->received_payload, request_payload);

    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: burst delivery
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, BurstDelivery) {
    const std::vector<std::string> burst_payloads = {
        "MESSAGE_ONE", "MESSAGE_TWO", "MESSAGE_THREE",
        "MESSAGE_FOUR", "MESSAGE_FIVE"
    };
    const int expected_count = static_cast<int>(burst_payloads.size());

    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<BurstListenerThread>(
        logger_->logger, *listener_reactor, expected_count);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Send all messages back-to-back as a single burst.
    for (const auto& payload : burst_payloads) {
        const std::string frame = make_framed(payload);
        ASSERT_TRUE(send_all(sock_fd, frame.data(), frame.size()))
            << "Failed to send burst message: " << payload;
    }

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->all_received.load(std::memory_order_acquire);
    })) << "Listener: did not receive all burst messages within timeout";

    ASSERT_EQ(static_cast<int>(listener_thread->received_payloads.size()), expected_count);
    for (int i = 0; i < expected_count; ++i) {
        EXPECT_EQ(listener_thread->received_payloads[i], burst_payloads[i])
            << "Mismatch at burst message index " << i;
    }

    ::close(sock_fd);
    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: peer disconnect detected by listener (exercises recv==0 path)
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, PeerDisconnect) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Close the socket -- sends FIN, triggering recv==0 in on_data_ready().
    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer closed socket";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: one-connection-per-listener enforcement
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, OneConnectionRejection) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    // First connection -- must be accepted.
    const int first_fd = connect_raw_socket(listen_port);
    ASSERT_NE(first_fd, -1) << "First socket failed to connect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received for first connection";

    // Second connection -- must be silently rejected.
    const int second_fd = connect_raw_socket(listen_port);
    ASSERT_NE(second_fd, -1) << "Second socket failed to connect at TCP level";

    // Give the reactor time to process and silently close the second socket.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Only one ConnectionEstablished must have been delivered.
    EXPECT_EQ(listener_thread->connection_established_count.load(std::memory_order_acquire), 1)
        << "Listener received more than one ConnectionEstablished -- rejection failed";

    // The first connection must still be alive.
    EXPECT_FALSE(listener_thread->connection_lost.load(std::memory_order_acquire))
        << "First connection was incorrectly lost after second connector arrived";

    ::close(second_fd);
    ::close(first_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after closing first socket";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: idle timeout teardown
// ============================================================
TEST_F(RawBytesProtocolHandlerTest, IdleTimeout) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_short_idle_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfig{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = std::make_shared<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    // Connect and send nothing -- the connection will go idle.
    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Idle timeout is 300ms, check interval 50ms. Allow 2 seconds total.
    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    }, 2000)) << "Listener: ConnectionLost not received after idle timeout";

    ::close(sock_fd);
    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

} // namespace pubsub_itc_fw::tests
