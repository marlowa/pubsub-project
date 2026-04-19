// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OutboundConnectionTest.cpp
 * @brief Integration and unit tests for outbound connection error paths.
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
 *
 *   ConstructorRejectsNullConnector
 *     Verifies that constructing an OutboundConnection with a null connector
 *     throws PreconditionAssertion.
 *
 *   OnConnectedRejectsNullSocket
 *     Verifies that on_connected() with a null socket throws PreconditionAssertion.
 *
 *   OnConnectedRejectsWhenNotConnecting
 *     Verifies that calling on_connected() a second time (when already in the
 *     established phase) throws PreconditionAssertion.
 *
 *   RetryWithSecondaryRejectsNullConnector
 *     Verifies that retry_with_secondary() with a null connector throws
 *     PreconditionAssertion.
 *
 *   SetAndClearPendingSend
 *     Verifies the set_pending_send() / clear_pending_send() / has_pending_send()
 *     state management and accessor correctness.
 *
 *   SetPendingSendRejectsNullAllocator
 *     Verifies that set_pending_send() with a null allocator throws
 *     PreconditionAssertion.
 *
 *   SetPendingSendRejectsNullChunkPtr
 *     Verifies that set_pending_send() with a null chunk_ptr throws
 *     PreconditionAssertion.
 *
 *   TeardownWithPendingSendFreesChunk
 *     Establishes a real outbound connection via loopback, injects pending-send
 *     state directly via set_pending_send(), then calls teardown_connection()
 *     via the Reactor::outbound_manager() test seam. Verifies the chunk is
 *     freed, covering the conn.has_pending_send() branch in
 *     OutboundConnectionManager::teardown_connection (lines 451-452).
 *
 *   DrainPendingSendDispatchesStashedCommand
 *     Establishes a real outbound connection, sends a large PDU to fill the
 *     kernel send buffer so has_pending_data() is true, then calls
 *     process_send_pdu_command() with a second PDU while the first is still
 *     blocked — this stashes the second into pending_send_. Drains the peer
 *     socket, drives on_write_ready() to complete the first send, then calls
 *     drain_pending_send() directly. Covers the drain_pending_send body
 *     (lines 364-375).
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
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
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/OutboundConnection.hpp>
#include <pubsub_itc_fw/OutboundConnectionManager.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/TcpConnector.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
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
// Integration test fixture
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
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1) << "Failed to create listening socket";

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
        ::close(tmp_fd);
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

    ServiceRegistry registry;

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

// ============================================================
// Unit test fixture: OutboundConnection preconditions and state.
// ============================================================
class OutboundConnectionPreconditionTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>(
            "outbound_unit_logger", "outbound_unit_sink");

        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);

        reactor_ = std::make_unique<Reactor>(cfg, registry_, logger_->logger);
        thread_  = std::make_shared<OutboundTestThread>(
            logger_->logger, *reactor_, "dummy_service");
        allocator_ = std::make_unique<ExpandableSlabAllocator>(65536);
    }

    void TearDown() override {
        allocator_.reset();
        thread_.reset();
        reactor_.reset();
        logger_.reset();
    }

    std::unique_ptr<OutboundConnection> make_connection() {
        auto connector = std::make_unique<TcpConnector>();
        ServiceEndpoints endpoints{
            NetworkEndpointConfiguration{"127.0.0.1", 9999},
            NetworkEndpointConfiguration{}
        };
        return std::make_unique<OutboundConnection>(
            ConnectionID{1},
            ThreadID{1},
            "dummy_service",
            endpoints,
            std::move(connector),
            *allocator_,
            *thread_);
    }

    std::unique_ptr<LoggerWithSink>          logger_;
    ServiceRegistry                          registry_;
    std::unique_ptr<Reactor>                 reactor_;
    std::shared_ptr<OutboundTestThread>      thread_;
    std::unique_ptr<ExpandableSlabAllocator> allocator_;
};

TEST_F(OutboundConnectionPreconditionTest, ConstructorRejectsNullConnector) {
    ServiceEndpoints endpoints{
        NetworkEndpointConfiguration{"127.0.0.1", 9999},
        NetworkEndpointConfiguration{}
    };
    EXPECT_THROW(
        OutboundConnection(
            ConnectionID{1}, ThreadID{1}, "dummy_service",
            endpoints, nullptr, *allocator_, *thread_),
        PreconditionAssertion);
}

TEST_F(OutboundConnectionPreconditionTest, OnConnectedRejectsNullSocket) {
    auto conn = make_connection();
    EXPECT_THROW(conn->on_connected(nullptr, [](){}), PreconditionAssertion);
}

TEST_F(OutboundConnectionPreconditionTest, OnConnectedRejectsWhenNotConnecting) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    auto conn = make_connection();

    auto [socket, err1] = TcpSocket::adopt(fds[0]);
    ASSERT_NE(socket, nullptr) << "Failed to adopt socket: " << err1;
    EXPECT_NO_THROW(conn->on_connected(std::move(socket), [](){}));
    EXPECT_TRUE(conn->is_established());
    EXPECT_FALSE(conn->is_connecting());

    auto [socket2, err2] = TcpSocket::adopt(fds[1]);
    ASSERT_NE(socket2, nullptr) << "Failed to adopt socket2: " << err2;
    EXPECT_THROW(conn->on_connected(std::move(socket2), [](){}), PreconditionAssertion);
}

TEST_F(OutboundConnectionPreconditionTest, RetryWithSecondaryRejectsNullConnector) {
    auto conn = make_connection();
    EXPECT_THROW(conn->retry_with_secondary(nullptr), PreconditionAssertion);
}

TEST_F(OutboundConnectionPreconditionTest, SetAndClearPendingSend) {
    auto conn = make_connection();

    EXPECT_FALSE(conn->has_pending_send());

    char dummy_chunk[64]{};
    conn->set_pending_send(allocator_.get(), 0, dummy_chunk, sizeof(dummy_chunk));

    EXPECT_TRUE(conn->has_pending_send());
    EXPECT_EQ(conn->current_allocator(), allocator_.get());
    EXPECT_EQ(conn->current_slab_id(), 0);
    EXPECT_EQ(conn->current_chunk_ptr(), static_cast<void*>(dummy_chunk));
    EXPECT_EQ(conn->current_total_bytes(), static_cast<uint32_t>(sizeof(dummy_chunk)));

    conn->clear_pending_send();

    EXPECT_FALSE(conn->has_pending_send());
    EXPECT_EQ(conn->current_allocator(), nullptr);
    EXPECT_EQ(conn->current_slab_id(), -1);
    EXPECT_EQ(conn->current_chunk_ptr(), nullptr);
    EXPECT_EQ(conn->current_total_bytes(), 0u);
}

TEST_F(OutboundConnectionPreconditionTest, SetPendingSendRejectsNullAllocator) {
    auto conn = make_connection();
    char dummy_chunk[64]{};
    EXPECT_THROW(
        conn->set_pending_send(nullptr, 0, dummy_chunk, sizeof(dummy_chunk)),
        PreconditionAssertion);
}

TEST_F(OutboundConnectionPreconditionTest, SetPendingSendRejectsNullChunkPtr) {
    auto conn = make_connection();
    EXPECT_THROW(
        conn->set_pending_send(allocator_.get(), 0, nullptr, 64),
        PreconditionAssertion);
}

// ============================================================
// Fixture for OutboundConnectionManager pending-send path tests.
//
// Uses the Reactor::outbound_manager() test seam to drive the manager
// directly without a running event loop. A real loopback listener is
// used to establish the connection deterministically. The reactor is
// constructed but never run — we call process_connect_command and
// on_connect_ready directly on the manager to transition the connection
// to established phase under full test control.
// ============================================================
class OutboundConnectionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>(
            "outbound_mgr_logger", "outbound_mgr_sink");

        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.shutdown_timeout_          = std::chrono::milliseconds(1000);
        cfg.connect_timeout            = std::chrono::milliseconds(2000);

        reactor_ = std::make_unique<Reactor>(cfg, registry_, logger_->logger);

        // Register stub thread. fast_path_threads_ is not populated since we
        // never call run() — but deliver_lost_event=false means teardown_connection
        // never calls get_fast_path_thread, so this is safe.
        stub_thread_ = std::make_shared<OutboundTestThread>(
            logger_->logger, *reactor_, "test_service");
        reactor_->register_thread(stub_thread_);

        outbound_allocator_ = std::make_unique<ExpandableSlabAllocator>(4 * 1024 * 1024);
    }

    void TearDown() override {
        if (accept_thread_.joinable()) accept_thread_.join();
        outbound_allocator_.reset();
        stub_thread_.reset();
        reactor_.reset();
        logger_.reset();
    }

    // Start a loopback listener and accept one connection in the background.
    // Returns the listen port. The accepted peer fd is stored in peer_fd_.
    uint16_t start_listener_and_accept() {
        auto [listen_fd, port] = make_listener();
        if (listen_fd == -1) return 0;

        accept_thread_ = std::thread([this, listen_fd]() {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            peer_fd_ = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
            ::close(listen_fd);
            accepted_.store(true, std::memory_order_release);
        });

        return port;
    }

    // Drive process_connect_command then on_connect_ready to get a fully
    // established OutboundConnection in the manager. Returns a pointer to
    // the connection (owned by the manager), or nullptr on failure.
    OutboundConnection* establish(uint16_t port, ConnectionID id) {
        registry_.add("test_service",
            NetworkEndpointConfiguration{"127.0.0.1", port},
            NetworkEndpointConfiguration{});

        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Connect);
        cmd.requesting_thread_id_ = ThreadID{1};
        cmd.service_name_         = "test_service";

        OutboundConnectionManager& mgr = reactor_->outbound_manager();
        mgr.process_connect_command(cmd, id);

        // Loopback connects complete within microseconds.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Find the connecting connection by scanning fds via find_by_fd.
        OutboundConnection* conn = nullptr;
        for (int fd = 3; fd < 1024 && conn == nullptr; ++fd) {
            OutboundConnection* candidate = mgr.find_by_fd(fd);
            if (candidate != nullptr && candidate->id() == id) {
                conn = candidate;
            }
        }
        if (conn == nullptr || !conn->is_connecting()) return nullptr;

        mgr.on_connect_ready(*conn);

        // After on_connect_ready the fd changes (connector fd → socket fd).
        conn = nullptr;
        for (int fd = 3; fd < 1024 && conn == nullptr; ++fd) {
            OutboundConnection* candidate = mgr.find_by_fd(fd);
            if (candidate != nullptr && candidate->id() == id) {
                conn = candidate;
            }
        }
        return (conn != nullptr && conn->is_established()) ? conn : nullptr;
    }

    // Build a pre-assembled PDU frame in the outbound allocator.
    struct Frame {
        int      slab_id;
        void*    chunk;
        uint32_t total_bytes;
    };

    Frame make_frame(size_t payload_size) {
        const uint32_t total = static_cast<uint32_t>(sizeof(PduHeader) + payload_size);
        auto [slab_id, chunk] = outbound_allocator_->allocate(total);
        auto* hdr = static_cast<PduHeader*>(chunk);
        hdr->byte_count = htonl(static_cast<uint32_t>(payload_size));
        hdr->pdu_id     = htons(static_cast<uint16_t>(42));
        hdr->version    = 1;
        hdr->filler_a   = 0;
        hdr->canary     = htonl(pdu_canary_value);
        hdr->filler_b   = 0;
        std::memset(static_cast<uint8_t*>(chunk) + sizeof(PduHeader), 0xAB, payload_size);
        return {slab_id, chunk, total};
    }

    std::unique_ptr<LoggerWithSink>          logger_;
    ServiceRegistry                          registry_;
    std::unique_ptr<Reactor>                 reactor_;
    std::shared_ptr<OutboundTestThread>      stub_thread_;
    std::unique_ptr<ExpandableSlabAllocator> outbound_allocator_;
    std::thread                              accept_thread_;
    std::atomic<bool>                        accepted_{false};
    int                                      peer_fd_{-1};
};

// ============================================================
// Test: teardown_connection frees the in-flight slab chunk when
// conn.has_pending_send() is true (covers lines 451-452 in
// OutboundConnectionManager::teardown_connection).
// ============================================================
TEST_F(OutboundConnectionManagerTest, TeardownWithPendingSendFreesChunk) {
    const uint16_t port = start_listener_and_accept();
    ASSERT_NE(port, 0u) << "Failed to start listener";

    const ConnectionID conn_id{42};
    OutboundConnection* conn = establish(port, conn_id);

    if (accept_thread_.joinable()) accept_thread_.join();
    if (peer_fd_ != -1) { ::close(peer_fd_); peer_fd_ = -1; }

    ASSERT_NE(conn, nullptr) << "Failed to establish outbound connection";
    ASSERT_TRUE(conn->is_established());

    // Allocate a real slab chunk to act as the in-flight frame.
    auto [slab_id, chunk, total_bytes] = make_frame(128);
    ASSERT_NE(chunk, nullptr);

    // Inject pending-send state directly — no real partial write needed.
    conn->set_pending_send(outbound_allocator_.get(), slab_id, chunk, total_bytes);
    ASSERT_TRUE(conn->has_pending_send());

    const size_t slabs_before = outbound_allocator_->slab_count();

    // Call teardown_connection. It must free the chunk via
    // conn.current_allocator()->deallocate() (the uncovered lines 451-452).
    // deliver_lost_event=false so fast_path_threads_ is not accessed.
    reactor_->outbound_manager().teardown_connection(conn_id, "test teardown", false);

    // Verify the chunk was freed: allocate the same size again — the slab
    // count must not have grown beyond what it was before teardown.
    auto [slab_id2, chunk2] = outbound_allocator_->allocate(128);
    EXPECT_NE(chunk2, nullptr);
    EXPECT_LE(outbound_allocator_->slab_count(), slabs_before)
        << "Slab count grew after teardown — chunk was not freed by teardown_connection";
    outbound_allocator_->deallocate(slab_id2, chunk2);
}

// ============================================================
// Test: drain_pending_send dispatches the stashed command (covers
// lines 364-375 in OutboundConnectionManager::drain_pending_send).
//
// Strategy:
//   1. Establish connection. Set a tiny SO_SNDBUF to guarantee blocking.
//   2. Send a large PDU — has_pending_data() stays true, set_pending_send
//      is called by process_send_pdu_command.
//   3. Send a second PDU while the first is still blocked — it gets stashed
//      into pending_send_ by process_send_pdu_command.
//   4. Drain the peer socket and call on_write_ready() until the first send
//      completes and conn.has_pending_send() becomes false.
//   5. Call drain_pending_send() — it must process the stashed second command,
//      covering the body of drain_pending_send (lines 364-375).
// ============================================================
TEST_F(OutboundConnectionManagerTest, DrainPendingSendDispatchesStashedCommand) {
    const uint16_t port = start_listener_and_accept();
    ASSERT_NE(port, 0u) << "Failed to start listener";

    const ConnectionID conn_id{43};
    OutboundConnection* conn = establish(port, conn_id);

    if (accept_thread_.joinable()) accept_thread_.join();

    ASSERT_NE(conn, nullptr) << "Failed to establish outbound connection";
    ASSERT_TRUE(conn->is_established());

    // Set a tiny send buffer to force the large send to block.
    const int tiny_sndbuf = 4096;
    ::setsockopt(conn->get_fd(), SOL_SOCKET, SO_SNDBUF, &tiny_sndbuf, sizeof(tiny_sndbuf));

    // Send a large PDU. With a tiny buffer it should block immediately.
    constexpr size_t large_payload = 256 * 1024;
    auto [slab_id1, chunk1, total1] = make_frame(large_payload);

    OutboundConnectionManager& mgr = reactor_->outbound_manager();

    ReactorControlCommand cmd1(ReactorControlCommand::CommandTag::SendPdu);
    cmd1.connection_id_  = conn_id;
    cmd1.allocator_      = outbound_allocator_.get();
    cmd1.slab_id_        = slab_id1;
    cmd1.pdu_chunk_ptr_  = chunk1;
    cmd1.pdu_byte_count_ = static_cast<uint32_t>(large_payload);
    ASSERT_TRUE(mgr.process_send_pdu_command(cmd1));

    if (!conn->has_pending_send()) {
        // Kernel absorbed the full frame — can't test drain_pending_send here.
        if (peer_fd_ != -1) { ::close(peer_fd_); peer_fd_ = -1; }
        mgr.teardown_connection(conn_id, "cleanup", false);
        GTEST_SKIP() << "Kernel send buffer absorbed the full frame — skipping";
    }

    // Send a second PDU while the first is still blocked.
    // process_send_pdu_command sees conn.has_pending_send() == true and
    // stashes it into pending_send_.
    auto [slab_id2, chunk2, total2] = make_frame(128);

    ReactorControlCommand cmd2(ReactorControlCommand::CommandTag::SendPdu);
    cmd2.connection_id_  = conn_id;
    cmd2.allocator_      = outbound_allocator_.get();
    cmd2.slab_id_        = slab_id2;
    cmd2.pdu_chunk_ptr_  = chunk2;
    cmd2.pdu_byte_count_ = 128;
    ASSERT_TRUE(mgr.process_send_pdu_command(cmd2));

    // Drain the peer socket and drive on_write_ready until the first send
    // completes.
    if (peer_fd_ != -1) {
        const int flags = ::fcntl(peer_fd_, F_GETFL, 0);
        ::fcntl(peer_fd_, F_SETFL, flags | O_NONBLOCK);

        std::vector<uint8_t> buf(65536);
        for (int i = 0; i < 100000 && conn->has_pending_send(); ++i) {
            ::read(peer_fd_, buf.data(), buf.size());
            mgr.on_write_ready(*conn);
        }
    }

    ASSERT_FALSE(conn->has_pending_send())
        << "First send did not complete after draining peer";

    // drain_pending_send must now process the stashed second command,
    // covering the drain_pending_send body (lines 364-375).
    const bool drained = mgr.drain_pending_send();
    EXPECT_TRUE(drained);

    // Clean up.
    if (peer_fd_ != -1) { ::close(peer_fd_); peer_fd_ = -1; }
    mgr.teardown_connection(conn_id, "test complete", false);
}

} // namespace pubsub_itc_fw::tests
