// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file RawBytesProtocolHandlerIntegrationTest.cpp
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
 * MirroredBuffer commit contract:
 *   RawBytesProtocolHandler calls on_raw_socket_message() every time new
 *   bytes arrive on the socket. Each call receives a view of ALL currently
 *   unprocessed bytes starting from the tail -- not just the newly arrived
 *   bytes. The tail only advances when the reactor processes a CommitRawBytes
 *   command, which happens asynchronously. Between two on_raw_socket_message()
 *   calls, if the tail has not yet advanced, payload() points to the same
 *   start address and payload_size() may be larger (old bytes + new bytes).
 *
 *   Each EventMessage carries the MirroredBuffer tail position at enqueue time
 *   via tail_position(). The application thread compares this against its last
 *   seen tail to detect unambiguously whether the tail advanced between
 *   deliveries. When it has, bytes_decoded_ is reset to 0 so decoding starts
 *   fresh from the new tail position. This replaces the fragile
 *   available < last_available_ heuristic which breaks when new data arrives
 *   simultaneously with a commit being processed.
 *
 *   Commit policy: only commit when the entire current window has been
 *   consumed (bytes_decoded_ == available). If a partial message remains,
 *   do not commit -- wait for the next EPOLLIN which delivers the partial
 *   bytes together with whatever new bytes arrive. A rogue client that sends
 *   a partial message and goes silent will eventually fill the buffer and be
 *   disconnected by the framework's backpressure mechanism.
 *
 * Tests in this file:
 *
 *   RawByteRoundTrip
 *     Happy-path: raw socket sends one framed message, listener replies via
 *     send_raw(), raw socket reads and verifies the reply.
 *
 *   FragmentedDelivery
 *     The raw socket sends the 4-byte length prefix and the payload bytes in
 *     two separate ::send() calls with a short sleep between them. The listener
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
 *   MultipleConnectionsAccepted
 *     Verifies that FrameworkPdu listeners now accept multiple concurrent
 *     connections. The old one-connection-per-listener rule has been removed.
 *     Both connections receive ConnectionEstablished with no interference.
 *
 *   IdleTimeout
 *     The listener is configured with a very short idle timeout. The raw socket
 *     connects and sends nothing. After the interval the reactor must tear down
 *     the connection and deliver ConnectionLost.
 *
 *   LargeReplyContinueSend
 *     The listener replies with a 512 KB frame while the raw socket has a tiny
 *     SO_RCVBUF (4096 bytes), forcing the outbound write to block. The reactor
 *     must call RawBytesProtocolHandler::continue_send() on EPOLLOUT until the
 *     frame drains. The raw socket then reads all bytes and verifies the length.
 *     Covers RawBytesProtocolHandler::continue_send().
 *
 *   TeardownWhilePendingSendFreesChunk
 *     The listener replies with a large frame (tiny SO_RCVBUF), then the raw
 *     socket closes immediately without reading. The reactor detects EPOLLERR or
 *     peer-closed, calls teardown_connection while has_pending_send() is true,
 *     which calls RawBytesProtocolHandler::deallocate_pending_send() to free
 *     the in-flight slab chunk. Covers deallocate_pending_send().
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

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

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

static std::string make_framed(const std::string& payload) {
    const uint32_t length_be = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame(length_prefix_size + payload.size(), '\0');
    std::memcpy(frame.data(), &length_be, length_prefix_size);
    std::memcpy(frame.data() + length_prefix_size, payload.data(), payload.size());
    return frame;
}

static bool send_all(int sock_fd, const void* buf, std::size_t size) {
    const auto* ptr = static_cast<const char*>(buf);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t sent = ::send(sock_fd, ptr, remaining, 0);
        if (sent <= 0) return false;
        ptr       += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

static bool recv_all(int sock_fd, void* buf, std::size_t size) {
    auto* ptr = static_cast<char*>(buf);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t received = ::recv(sock_fd, ptr, remaining, 0);
        if (received <= 0) return false;
        ptr       += received;
        remaining -= static_cast<std::size_t>(received);
    }
    return true;
}

static std::string recv_framed(int sock_fd) {
    uint32_t length_be = 0;
    if (!recv_all(sock_fd, &length_be, sizeof(length_be))) return {};
    const uint32_t payload_length = ntohl(length_be);
    std::string payload(payload_length, '\0');
    if (!recv_all(sock_fd, payload.data(), payload_length)) return {};
    return payload;
}

static int connect_raw_socket(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return -1;
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
// Test protocol decode helpers
// ============================================================

static std::string try_decode_framed(const uint8_t* data, int available,
                                     int64_t& bytes_consumed) {
    bytes_consumed = 0;
    if (available < static_cast<int>(length_prefix_size)) return {};
    uint32_t length_be = 0;
    std::memcpy(&length_be, data, length_prefix_size);
    const uint32_t payload_length = ntohl(length_be);
    const int64_t total = static_cast<int64_t>(length_prefix_size + payload_length);
    if (available < static_cast<int>(total)) return {};
    std::string payload(reinterpret_cast<const char*>(data + length_prefix_size),
                        payload_length);
    bytes_consumed = total;
    return payload;
}

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
        if (consumed == 0) break;
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
    RawListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "RawListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("RawListenerPool"),
                            ApplicationThreadConfiguration{}) {}

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

        if (data == nullptr || available <= 0) return;

        // If available shrank, the reactor processed our commit and the tail
        // advanced -- reset decode tracking.
        if (available < last_available_) {
            bytes_decoded_ = 0;
        }
        last_available_ = available;

        const int unprocessed = available - bytes_decoded_;
        if (unprocessed <= 0) return;

        int64_t bytes_consumed = 0;
        std::string payload = try_decode_framed(
            data + bytes_decoded_, unprocessed, bytes_consumed);
        if (bytes_consumed == 0) return; // incomplete -- wait for more data

        received_payload = payload;
        message_received.store(true, std::memory_order_release);

        bytes_decoded_ += static_cast<int>(bytes_consumed);
        commit_raw_bytes(conn_id, static_cast<int64_t>(bytes_decoded_));

        const std::string reply = make_framed(response_payload);
        send_raw(conn_id, reply.data(), static_cast<uint32_t>(reply.size()));
        reply_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    int bytes_decoded_{0};
    int last_available_{0};
};

// ============================================================
// Listener thread: decodes multiple messages from a burst.
// Used by BurstDelivery.
// ============================================================
class BurstListenerThread : public ApplicationThread {
public:
    BurstListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int expected_count)
        : ApplicationThread(token, logger, reactor, "BurstListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("BurstListenerPool"),
                            ApplicationThreadConfiguration{})
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
        const uint8_t* data      = message.payload();
        const int      available = message.payload_size();
        const int64_t  tail      = message.tail_position();

        if (data == nullptr || available <= 0) return;

        // Exact tail-advance detection: if the tail moved since the last
        // delivery, start decoding from the beginning of the new window.
        if (tail != last_tail_) {
            bytes_decoded_  = 0;
            last_committed_ = 0;
            last_tail_      = tail;
        }

        const int unprocessed = available - bytes_decoded_;
        if (unprocessed <= 0) return;

        int64_t total_consumed = 0;
        decode_all_framed(data + bytes_decoded_, unprocessed,
                          received_payloads, total_consumed);

        if (total_consumed > 0) {
            bytes_decoded_ += static_cast<int>(total_consumed);
            // Only commit when the entire current window is consumed.
            // If partial bytes remain (bytes_decoded_ < available) leave them
            // in the buffer -- the next EPOLLIN delivers them with new bytes.
            // A rogue client sending a partial message and going silent will
            // eventually fill the buffer and be disconnected by the framework.
            if (bytes_decoded_ == available) {
                const int64_t delta = static_cast<int64_t>(bytes_decoded_ - last_committed_);
                commit_raw_bytes(conn_id, delta);
                last_committed_ = bytes_decoded_;
            }
        }

        if (static_cast<int>(received_payloads.size()) >= expected_count_) {
            all_received.store(true, std::memory_order_release);
        }
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    int     expected_count_;
    int     bytes_decoded_{0};
    int     last_committed_{0};
    int64_t last_tail_{-1};
};

// ============================================================
// Listener thread: tracks connection events only, does not consume bytes.
// Used by PeerDisconnect, MultipleConnectionsAccepted, and IdleTimeout.
// ============================================================
class PassiveListenerThread : public ApplicationThread {
public:
    PassiveListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "PassiveListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("PassiveListenerPool"),
                            ApplicationThreadConfiguration{}) {}

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

    void on_raw_socket_message([[maybe_unused]] const EventMessage& msg) override {}
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// ============================================================
// Test fixture
// ============================================================
class RawBytesProtocolHandlerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
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
        if (reactor_thread.joinable()) reactor_thread.join();
    }

    std::unique_ptr<LoggerWithSink> logger_;
};

// ============================================================
// Test: happy-path round-trip
// ============================================================
TEST_F(RawBytesProtocolHandlerIntegrationTest, RawByteRoundTrip) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<RawListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    const std::string frame = make_framed(request_payload);
    ASSERT_TRUE(send_all(sock_fd, frame.data(), frame.size()))
        << "Failed to send framed request";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: reply not sent";

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
TEST_F(RawBytesProtocolHandlerIntegrationTest, FragmentedDelivery) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<RawListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    const int one = 1;
    ::setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    const uint32_t length_be = htonl(static_cast<uint32_t>(request_payload.size()));
    ASSERT_TRUE(send_all(sock_fd, &length_be, sizeof(length_be)))
        << "Failed to send length prefix";

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

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
TEST_F(RawBytesProtocolHandlerIntegrationTest, BurstDelivery) {
    const std::vector<std::string> burst_payloads = {
        "MESSAGE_ONE", "MESSAGE_TWO", "MESSAGE_THREE",
        "MESSAGE_FOUR", "MESSAGE_FIVE"
    };
    const int expected_count = static_cast<int>(burst_payloads.size());

    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<BurstListenerThread>(
        logger_->logger, *listener_reactor, expected_count);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

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
TEST_F(RawBytesProtocolHandlerIntegrationTest, PeerDisconnect) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer closed socket";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: FrameworkPdu listener accepts multiple concurrent connections.
//
// Previously the framework enforced a one-connection-per-listener rule for
// FrameworkPdu listeners. This has been removed -- all listeners now accept
// any number of concurrent connections. This test verifies the new behaviour
// by connecting two clients to a FrameworkPdu listener and confirming that
// both receive ConnectionEstablished and that the first is not disturbed by
// the second.
// ============================================================
TEST_F(RawBytesProtocolHandlerIntegrationTest, MultipleConnectionsAccepted) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_reactor_config(), listener_registry, logger_->logger);

    // FrameworkPdu listener -- multiple connections now accepted.
    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto listener_thread = ApplicationThread::create<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int first_fd = connect_raw_socket(listen_port);
    ASSERT_NE(first_fd, -1) << "First socket failed to connect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established_count.load(std::memory_order_acquire) >= 1;
    })) << "Listener: ConnectionEstablished not received for first connection";

    const int second_fd = connect_raw_socket(listen_port);
    ASSERT_NE(second_fd, -1) << "Second socket failed to connect";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established_count.load(std::memory_order_acquire) >= 2;
    })) << "Listener: second ConnectionEstablished not received -- multiple connections not accepted";

    EXPECT_FALSE(listener_thread->connection_lost.load(std::memory_order_acquire))
        << "A connection was lost unexpectedly while both clients were connected";

    ::close(second_fd);
    ::close(first_fd);

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: idle timeout teardown
// ============================================================
TEST_F(RawBytesProtocolHandlerIntegrationTest, IdleTimeout) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(
        make_short_idle_reactor_config(), listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<PassiveListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    }, 2000)) << "Listener: ConnectionLost not received after idle timeout";

    ::close(sock_fd);
    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Listener thread: on receiving any message sends a large reply
// (512 KB) to force a partial send. Used by LargeReplyContinueSend
// and TeardownWhilePendingSendFreesChunk.
// ============================================================

static constexpr size_t large_reply_payload_size = 512 * 1024;
static constexpr int    tiny_rcvbuf_size         = 4096;

class LargeReplyListenerThread : public ApplicationThread {
public:
    LargeReplyListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "LargeReplyListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("LargeReplyListenerPool"),
                            make_thread_config()) {}

private:
    static ApplicationThreadConfiguration make_thread_config() {
        ApplicationThreadConfiguration cfg{};
        cfg.outbound_slab_size = 2 * 1024 * 1024;
        return cfg;
    }

public:

    std::atomic<bool> connection_established{false};
    std::atomic<bool> reply_sent{false};
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

    void on_raw_socket_message(const EventMessage& message) override {
        // Commit the incoming bytes immediately so the buffer stays clear.
        commit_raw_bytes(conn_id, message.payload_size());

        if (reply_sent.load(std::memory_order_acquire)) return;

        // Send a 512 KB reply — large enough to overflow a 4096-byte SO_RCVBUF
        // and force framer_->has_pending_data() to be true, exercising
        // continue_send() on EPOLLOUT and deallocate_pending_send() on teardown.
        large_reply_.assign(large_reply_payload_size, 'Z');
        send_raw(conn_id, large_reply_.data(),
                 static_cast<uint32_t>(large_reply_.size()));
        reply_sent.store(true, std::memory_order_release);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}

private:
    std::string large_reply_;
};

// ============================================================
// Test: large reply forces partial send, exercising continue_send().
// ============================================================
TEST_F(RawBytesProtocolHandlerIntegrationTest, LargeReplyContinueSend) {
    ServiceRegistry listener_registry;

    // Small send buffer on the listener's accepted socket forces the 512 KB
    // reply to block, guaranteeing continue_send() is called on EPOLLOUT.
    ReactorConfiguration listener_cfg = make_reactor_config();
    listener_cfg.socket_send_buffer_size = tiny_rcvbuf_size;

    auto listener_reactor = std::make_unique<Reactor>(
        listener_cfg, listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<LargeReplyListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Send a small trigger message so the listener fires its large reply.
    const std::string trigger = make_framed(request_payload);
    ASSERT_TRUE(send_all(sock_fd, trigger.data(), trigger.size()))
        << "Failed to send trigger message";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: large reply not sent";

    // Drain all the reply bytes — this unblocks EPOLLOUT and drives continue_send().
    std::vector<uint8_t> drain_buf(65536);
    size_t total_received = 0;
    while (total_received < large_reply_payload_size) {
        const ssize_t n = ::recv(sock_fd, drain_buf.data(), drain_buf.size(), 0);
        if (n <= 0) break;
        total_received += static_cast<size_t>(n);
    }
    EXPECT_EQ(total_received, large_reply_payload_size)
        << "Did not receive the full large reply";

    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: peer closes while partial send is in flight, exercising
// deallocate_pending_send() via teardown_connection.
// ============================================================
TEST_F(RawBytesProtocolHandlerIntegrationTest, TeardownWhilePendingSendFreesChunk) {
    ServiceRegistry listener_registry;

    ReactorConfiguration listener_cfg = make_reactor_config();
    listener_cfg.socket_send_buffer_size = tiny_rcvbuf_size;

    auto listener_reactor = std::make_unique<Reactor>(
        listener_cfg, listener_registry, logger_->logger);

    listener_reactor->register_inbound_listener(
        NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
        ProtocolType{ProtocolType::RawBytes}, raw_buffer_capacity);

    auto listener_thread = ApplicationThread::create<LargeReplyListenerThread>(
        logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_raw_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect raw socket";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_established.load(std::memory_order_acquire);
    })) << "Listener: ConnectionEstablished not received";

    // Trigger the large reply.
    const std::string trigger = make_framed(request_payload);
    ASSERT_TRUE(send_all(sock_fd, trigger.data(), trigger.size()))
        << "Failed to send trigger message";

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->reply_sent.load(std::memory_order_acquire);
    })) << "Listener: large reply not sent";

    // Close immediately without draining — the listener's partial send is still
    // in flight. The reactor detects the peer-closed condition, calls
    // teardown_connection, which calls deallocate_pending_send() to free the
    // in-flight slab chunk (the uncovered function).
    ::close(sock_fd);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->connection_lost.load(std::memory_order_acquire);
    })) << "Listener: ConnectionLost not received after peer closed";

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

} // namespace pubsub_itc_fw::tests
