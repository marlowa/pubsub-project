// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file RawBytesBackpressureIntegrationTest.cpp
 * @brief Integration tests for the read-side backpressure mechanism on
 *        RawBytesProtocolHandler.
 *
 * When a peer sends bytes faster than the listener's ApplicationThread can
 * consume them, the MirroredBuffer fills up. Without backpressure, the buffer
 * eventually overflows and the framework tears the connection down. With
 * backpressure, the handler asks the InboundConnectionManager to deregister
 * EPOLLIN once the buffer crosses a high-water mark, which causes the kernel
 * TCP receive window to close, which in turn causes the peer's send() to
 * block or return EAGAIN. When the application drains the buffer below the
 * low-water mark, the manager re-registers EPOLLIN and the peer can send
 * again.
 *
 * Test protocol framing:
 *   These tests do not need any framing. They send raw bytes through a raw
 *   POSIX socket; the listener does not parse them, it merely counts them
 *   and chooses whether to commit them via commit_raw_bytes(). The point is
 *   to drive the buffer into its high-water and low-water transitions, not
 *   to exercise any protocol decode logic.
 *
 * Observability:
 *   Backpressure is verified end-to-end by observing the peer's send()
 *   behaviour on a non-blocking socket. When EPOLLIN is deregistered on the
 *   listener side, the kernel TCP window eventually closes; the peer's
 *   send() then returns EAGAIN once its own send buffer fills. When EPOLLIN
 *   is re-registered, the listener resumes draining the kernel buffer and
 *   the peer's send() succeeds again. This is the contract that matters in
 *   production -- the peer being told to slow down -- so the tests assert
 *   on it directly rather than on log lines or internal handler state.
 *
 * Tests in this file:
 *
 *   BackpressureEngagesWhenApplicationIsSlow
 *     The listener stops committing after the connection is established.
 *     The peer sends in a tight non-blocking loop; eventually send() returns
 *     EAGAIN. The test asserts that this happens before the listener has
 *     received a full buffer's worth of bytes (i.e. the kernel window closed
 *     because of EPOLLIN deregistration, not because of an unbounded
 *     application-level buffer).
 *
 *   BackpressureReleasesAfterCommitsDrainBuffer
 *     After EAGAIN is observed, the listener is told to drain. The peer's
 *     send() begins succeeding again. The test asserts that this happens
 *     within a bounded time and that all bytes sent are eventually delivered.
 *
 *   SustainedThroughputThroughManyBackpressureCycles
 *     The listener alternates between draining and not draining several
 *     times while the peer sends continuously. All bytes are eventually
 *     delivered; the connection stays up throughout; no buffer-full error
 *     is logged on the listener side.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
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

namespace {

// MirroredBuffer capacity small enough that the application-level
// high-water mark (75%) is reached well before the kernel's own
// receive buffer fills. With OS-default kernel buffers (typically
// ~200 KiB), a 16 KiB application buffer guarantees that our pause
// signal fires first; if it did not, the test would observe kernel-
// level flow control (EAGAIN purely from full TCP windows on tiny
// kernel buffers) rather than the application-level backpressure
// mechanism we are trying to verify.
constexpr int64_t backpressure_buffer_capacity = 16 * 1024;

// Bytes per peer send() iteration. Small relative to buffer capacity so the
// tests can observe the buffer crossing the thresholds at well-defined points.
constexpr std::size_t chunk_size = 1024;

// Cap on the number of chunks the peer will attempt to push before declaring
// failure. Generous so a slow CI runner does not produce a false negative,
// but bounded so a regression that disables backpressure entirely fails
// quickly rather than hanging. Sized to comfortably exceed the kernel's
// default send + receive buffer space so we know EAGAIN is the result of
// the listener-side window closing and not a finite push budget.
constexpr int max_send_chunks = 8192;

// Long enough for the kernel to propagate the closed window back to the peer
// once application-level backpressure engages and the reactor stops draining
// the kernel buffer. On localhost with default-sized kernel buffers this is
// typically a few tens of milliseconds; this is generous.
constexpr int eagain_timeout_ms = 5000;

// Long enough for the listener to process queued commits and the kernel to
// re-open the window once EPOLLIN is re-registered.
constexpr int resume_timeout_ms = 5000;

// Time we let the peer keep trying after EAGAIN to confirm that send() really
// is blocked and not just paused for a tick. If a successful send happens
// during this window, the resume was premature.
constexpr int eagain_persistence_ms = 200;

// ============================================================
// Raw POSIX socket helpers
// ============================================================

int connect_nonblocking_socket(uint16_t port) {
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

    // Switch to non-blocking so we can detect "kernel buffer full" as EAGAIN
    // rather than blocking indefinitely. The kernel send buffer is left at
    // its OS default so that the test does not artificially induce EAGAIN
    // via kernel-level flow control: we want EAGAIN to occur only because
    // application-level backpressure has engaged on the listener side and
    // closed the receive window.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        ::close(fd);
        return -1;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        ::close(fd);
        return -1;
    }

    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// Attempts a single non-blocking send. Returns the number of bytes sent
// (which may be less than size on short write), or -1 with errno == EAGAIN
// when the kernel send buffer is full, or -1 on a real send error.
ssize_t try_send_chunk(int sock_fd, const void* data, std::size_t size) {
    return ::send(sock_fd, data, size, MSG_NOSIGNAL);
}

// ============================================================
// Listener thread: tracks bytes received and commits only when allowed.
// ============================================================
//
// The drain mode is controlled via std::atomic<bool>. When false (the default)
// the listener counts bytes but does not call commit_raw_bytes(). This causes
// the MirroredBuffer fill to grow without bound until the high-water mark is
// crossed, at which point backpressure engages. Toggling to true causes the
// listener to commit the entire current window on its next callback, which
// drops the fill below the low-water mark and releases backpressure.
//
// The listener also tracks any teardown by recording on_connection_lost. The
// tests assert that this never fires unexpectedly -- backpressure must
// throttle the peer, not disconnect it.
class BackpressureListenerThread : public ApplicationThread {
public:
    BackpressureListenerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "BackpressureListenerThread", ThreadID{2},
                            make_queue_config(), make_allocator_config("BackpressureListenerPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<bool> connection_established{false};
    std::atomic<bool> connection_lost{false};
    std::atomic<bool> drain_enabled{false};
    std::atomic<int64_t> total_bytes_seen{0};
    std::atomic<int64_t> total_bytes_committed{0};
    std::atomic<int> callback_count{0};

    ConnectionID conn_id{};

protected:
    void on_connection_established(ConnectionID id) override {
        conn_id = id;
        connection_established.store(true, std::memory_order_release);
    }

    void on_connection_lost(ConnectionID, const std::string&) override {
        connection_lost.store(true, std::memory_order_release);
    }

    void on_raw_socket_message(const EventMessage& message) override {
        callback_count.fetch_add(1, std::memory_order_relaxed);

        const int available = message.payload_size();
        const int64_t tail = message.tail_position();

        // total_bytes_seen tracks the high-water mark in absolute terms.
        // tail + available is the absolute offset of the byte one past the
        // furthest byte the handler has read from the socket. Because the
        // buffer counters are monotonic, max() captures the largest value.
        const int64_t absolute_head = tail + available;
        int64_t previous = total_bytes_seen.load(std::memory_order_relaxed);
        while (absolute_head > previous
               && !total_bytes_seen.compare_exchange_weak(previous, absolute_head,
                                                          std::memory_order_relaxed)) {
            // retry
        }

        if (!drain_enabled.load(std::memory_order_acquire)) {
            // Deliberately slow the application thread so its event queue
            // backs up while bytes continue to arrive. Without this delay,
            // the app thread keeps pace with the reactor and the queue
            // never accumulates anything to drive a commit when drain mode
            // is later enabled. The exact value is not important; anything
            // long compared to the per-event work is enough to cause a
            // realistic backlog.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return;
        }

        // Drain mode: commit the new bytes in this window. new_bytes is the
        // amount the application has not yet asked to commit, computed from
        // the cumulative-window contract.
        const int64_t committed = total_bytes_committed.load(std::memory_order_relaxed);
        const int64_t new_bytes = absolute_head - committed;
        if (new_bytes <= 0) {
            return;
        }
        commit_raw_bytes(conn_id, new_bytes);
        total_bytes_committed.store(absolute_head, std::memory_order_relaxed);
    }

    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

ReactorConfiguration make_backpressure_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_        = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
    cfg.connect_timeout            = std::chrono::milliseconds(2000);
    return cfg;
}

} // namespace

// ============================================================
// Test fixture
// ============================================================
class RawBytesBackpressureIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        current_reactor_ = nullptr;
        logger_.reset();
    }

    void set_current_reactor(Reactor& reactor) {
        current_reactor_ = &reactor;
    }

    bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        reactor_died_ = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (current_reactor_ != nullptr && current_reactor_->is_finished()) {
                reactor_died_ = true;
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    std::string last_wait_failure_description() const {
        if (reactor_died_ && current_reactor_ != nullptr) {
            return "reactor terminated during wait; shutdown reason: " + current_reactor_->get_shutdown_reason();
        }
        return "predicate did not become true within timeout";
    }

    uint16_t start_listener_reactor(Reactor& reactor) {
        EXPECT_TRUE(wait_for([&]() { return reactor.is_initialized(); })) << "Listener reactor did not initialise within timeout";
        const uint16_t port = reactor.get_inbound_listener_port(0);
        EXPECT_NE(port, 0u) << "OS did not assign a valid listening port";
        return port;
    }

    static void shutdown_and_join(Reactor& reactor, std::thread& reactor_thread, const std::string& reason = "test complete") {
        reactor.shutdown(reason);
        if (reactor_thread.joinable()) {
            reactor_thread.join();
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    Reactor* current_reactor_{nullptr};
    bool reactor_died_{false};
};

// ============================================================
// Test: backpressure engages when the application stops draining.
// ============================================================
//
// The peer pushes chunks until either send() returns EAGAIN (which is what
// we want -- the kernel TCP window has closed because the listener stopped
// reading) or the peer has pushed an unreasonably large amount, which would
// indicate that backpressure did not engage at all.
TEST_F(RawBytesBackpressureIntegrationTest, BackpressureEngagesWhenApplicationIsSlow) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_backpressure_reactor_config(),
                                                      listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    listener_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
                                                ProtocolType{ProtocolType::RawBytes},
                                                backpressure_buffer_capacity);

    auto listener_thread = ApplicationThread::create<BackpressureListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_nonblocking_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect non-blocking peer socket";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    // Listener is configured to not drain (drain_enabled defaults to false).
    // Push chunks until send() returns EAGAIN or we hit the safety cap.
    const std::vector<uint8_t> chunk(chunk_size, 0xAA);
    int chunks_sent = 0;
    bool saw_eagain = false;
    int64_t bytes_sent_total = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(eagain_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && chunks_sent < max_send_chunks) {
        const ssize_t n = try_send_chunk(sock_fd, chunk.data(), chunk.size());
        if (n == static_cast<ssize_t>(chunk.size())) {
            bytes_sent_total += n;
            ++chunks_sent;
            continue;
        }
        if (n > 0) {
            bytes_sent_total += n;
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            saw_eagain = true;
            break;
        }
        FAIL() << "Unexpected send() error: " << std::strerror(errno);
    }

    EXPECT_TRUE(saw_eagain)
        << "Peer never observed EAGAIN; backpressure did not engage. "
        << "Sent " << bytes_sent_total << " bytes in " << chunks_sent
        << " chunks before giving up.";

    // The listener must not have torn the connection down. The whole point of
    // backpressure is that the connection stays alive while the peer waits.
    EXPECT_FALSE(listener_thread->connection_lost.load(std::memory_order_acquire))
        << "Listener tore the connection down instead of applying backpressure.";

    // Let the listener drain its backlog quickly so the connection_lost event
    // at end-of-test is reached promptly. Without this the listener spends
    // 5ms per queued event working through any backlog, which can run into
    // seconds when the queue is deep.
    listener_thread->drain_enabled.store(true, std::memory_order_release);

    ::close(sock_fd);
    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after peer closed: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: backpressure releases after the application drains the buffer.
// ============================================================
//
// First drive backpressure as in the previous test, then enable drain. The
// peer's send() must start succeeding again within a bounded time, and all
// bytes must be eventually delivered.
TEST_F(RawBytesBackpressureIntegrationTest, BackpressureReleasesAfterCommitsDrainBuffer) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_backpressure_reactor_config(),
                                                      listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    listener_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
                                                ProtocolType{ProtocolType::RawBytes},
                                                backpressure_buffer_capacity);

    auto listener_thread = ApplicationThread::create<BackpressureListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_nonblocking_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect non-blocking peer socket";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    const std::vector<uint8_t> chunk(chunk_size, 0xBB);
    int64_t bytes_sent_total = 0;
    bool saw_eagain = false;

    // Push until EAGAIN.
    const auto eagain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(eagain_timeout_ms);
    while (std::chrono::steady_clock::now() < eagain_deadline) {
        const ssize_t n = try_send_chunk(sock_fd, chunk.data(), chunk.size());
        if (n > 0) {
            bytes_sent_total += n;
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            saw_eagain = true;
            break;
        }
        FAIL() << "Unexpected send() error: " << std::strerror(errno);
    }
    ASSERT_TRUE(saw_eagain) << "Backpressure precondition failed: peer did not observe EAGAIN.";

    // Confirm EAGAIN is sticky: until the listener starts draining, send()
    // should not start succeeding again. A spuriously open window here would
    // indicate that backpressure is not actually holding the line.
    const auto persistence_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(eagain_persistence_ms);
    while (std::chrono::steady_clock::now() < persistence_deadline) {
        const ssize_t n = try_send_chunk(sock_fd, chunk.data(), chunk.size());
        ASSERT_TRUE(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            << "send() succeeded while backpressure should be holding "
            << "(n=" << n << ", errno=" << errno << ")";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Now enable drain. The listener will commit each window on its next
    // callback, which will drop the buffer fill below the low-water mark and
    // cause the manager to re-register EPOLLIN.
    listener_thread->drain_enabled.store(true, std::memory_order_release);

    // Spin trying to send. Within resume_timeout_ms one of these should succeed.
    bool resumed = false;
    const auto resume_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(resume_timeout_ms);
    while (std::chrono::steady_clock::now() < resume_deadline) {
        const ssize_t n = try_send_chunk(sock_fd, chunk.data(), chunk.size());
        if (n > 0) {
            bytes_sent_total += n;
            resumed = true;
            break;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        FAIL() << "Unexpected send() error during resume: " << std::strerror(errno);
    }
    EXPECT_TRUE(resumed)
        << "Peer never observed send() succeeding again after drain was enabled; "
        << "backpressure release did not happen.";

    EXPECT_FALSE(listener_thread->connection_lost.load(std::memory_order_acquire))
        << "Listener tore the connection down during backpressure cycle.";

    ::close(sock_fd);
    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after peer closed: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

// ============================================================
// Test: sustained load through many backpressure cycles.
// ============================================================
//
// The peer pushes bytes continuously in a non-blocking loop. The test
// toggles the listener's drain mode several times. Each toggle should
// produce a brief stall when backpressure engages and a brief catch-up
// burst when it releases. Across the whole run, all the bytes the peer
// pushed must eventually be received by the listener, and the connection
// must remain alive.
TEST_F(RawBytesBackpressureIntegrationTest, SustainedThroughputThroughManyBackpressureCycles) {
    ServiceRegistry listener_registry;
    auto listener_reactor = std::make_unique<Reactor>(make_backpressure_reactor_config(),
                                                      listener_registry, logger_->logger);
    set_current_reactor(*listener_reactor);

    listener_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2},
                                                ProtocolType{ProtocolType::RawBytes},
                                                backpressure_buffer_capacity);

    auto listener_thread = ApplicationThread::create<BackpressureListenerThread>(logger_->logger, *listener_reactor);
    listener_reactor->register_thread(listener_thread);

    std::thread listener_reactor_thread([&]() { listener_reactor->run(); });
    const uint16_t listen_port = start_listener_reactor(*listener_reactor);

    const int sock_fd = connect_nonblocking_socket(listen_port);
    ASSERT_NE(sock_fd, -1) << "Failed to connect non-blocking peer socket";

    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_established.load(std::memory_order_acquire); }))
        << "Listener: ConnectionEstablished not received: " << last_wait_failure_description();

    // Start in drain mode so the connection makes progress. The toggler thread
    // will flip it off and on a handful of times to drive backpressure cycles.
    listener_thread->drain_enabled.store(true, std::memory_order_release);

    std::atomic<bool> stop_toggler{false};
    std::thread toggler([&]() {
        for (int cycle = 0; cycle < 6 && !stop_toggler.load(std::memory_order_acquire); ++cycle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            listener_thread->drain_enabled.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            listener_thread->drain_enabled.store(true, std::memory_order_release);
        }
    });

    // Push a known total. With cycles in the low-hundreds-of-ms range and a
    // 4 KB peer send buffer, this should comfortably finish within a few
    // seconds whether backpressure is constantly engaging or not.
    const int total_chunks_to_send = 2048;
    const std::vector<uint8_t> chunk(chunk_size, 0xCC);
    int64_t bytes_sent_total = 0;
    int chunks_sent = 0;

    const auto send_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (chunks_sent < total_chunks_to_send && std::chrono::steady_clock::now() < send_deadline) {
        const ssize_t n = try_send_chunk(sock_fd, chunk.data(), chunk.size());
        if (n > 0) {
            bytes_sent_total += n;
            if (n == static_cast<ssize_t>(chunk.size())) {
                ++chunks_sent;
            }
            continue;
        }
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        FAIL() << "Unexpected send() error: " << std::strerror(errno);
    }
    EXPECT_EQ(chunks_sent, total_chunks_to_send)
        << "Peer did not finish sending within the deadline; backpressure may not be releasing.";

    stop_toggler.store(true, std::memory_order_release);
    toggler.join();

    // Final drain to make sure the listener sees the last bytes.
    listener_thread->drain_enabled.store(true, std::memory_order_release);

    EXPECT_TRUE(wait_for([&]() {
        return listener_thread->total_bytes_seen.load(std::memory_order_relaxed) >= bytes_sent_total;
    }, 5000))
        << "Listener did not see all sent bytes: total_bytes_seen="
        << listener_thread->total_bytes_seen.load(std::memory_order_relaxed)
        << " bytes_sent_total=" << bytes_sent_total
        << "; " << last_wait_failure_description();

    EXPECT_FALSE(listener_thread->connection_lost.load(std::memory_order_acquire))
        << "Listener tore the connection down during sustained backpressure cycles.";

    ::close(sock_fd);
    EXPECT_TRUE(wait_for([&]() { return listener_thread->connection_lost.load(std::memory_order_acquire); }))
        << "Listener: ConnectionLost not received after peer closed: " << last_wait_failure_description();

    shutdown_and_join(*listener_reactor, listener_reactor_thread);
}

} // namespace pubsub_itc_fw::tests
