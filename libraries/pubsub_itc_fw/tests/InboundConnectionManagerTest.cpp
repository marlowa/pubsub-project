// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file InboundConnectionManagerTest.cpp
 * @brief Tests for inbound connection teardown while a send is stashed.
 *
 * Tests in this file:
 *
 *   TeardownFreesAStashedRawSendRatherThanANullPduPointer
 *     Accepts a real loopback connection on a RawBytes listener, blocks its
 *     socket so a first raw send cannot complete, sends a second so that the
 *     command is stashed in pending_send_, and then tears the connection down
 *     as a peer reset would. This is the sequence that stopped the FIX order
 *     gateway on 2026-09-06 (BUG-0079): pending_send_ holds a whole
 *     ReactorControlCommand, whose SendPdu and SendRaw tags carry the chunk in
 *     different fields and leave the other null, and teardown_connection read
 *     the PDU field whatever the tag said. Every FIX byte to a member is sent
 *     raw, so the pointer was always null and the deallocate precondition
 *     always fired.
 *
 *   PausingAndResumingReadingSetTheConnectionsFlag,
 *   PausingOrResumingAConnectionThisManagerDoesNotHoldReportsFalse,
 *   ACommitOfBytesDoesNotUndoAnApplicationsPause,
 *   TheNewCommandsNameThemselves
 *     The mechanism behind ApplicationThread::pause_reading() and resume_reading(). Their effect on
 *     a running reactor is tested in integration_tests/PauseReadingIntegrationTest.cpp.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/InboundConnectionManager.hpp>
#include <pubsub_itc_fw/InboundListener.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;

namespace pubsub_itc_fw::tests {

// A thread only has to exist: on_accept refuses a connection whose target
// thread cannot be found, and teardown with SuppressLostEvent never delivers.
class InboundTestThread : public ApplicationThread {
  public:
    InboundTestThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "InboundTestThread", ThreadID{1}, make_queue_config(), make_allocator_config("InboundTestPool"),
                            ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {}
    void on_itc_message([[maybe_unused]] const EventMessage& msg) override {}
};

// Drives InboundConnectionManager directly through the Reactor::inbound_manager()
// test seam, with a real loopback connection and no running event loop, in the
// manner of OutboundConnectionManagerTest.
class InboundConnectionManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();

        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
        // Small enough that a modest send cannot drain into the kernel and the
        // handler is left with data pending. This is the lever the production
        // failure had by other means: a member that had stopped reading.
        cfg.socket_send_buffer_size = 4096;

        reactor_ = std::make_unique<Reactor>(cfg, registry_, logger_->logger);
        stub_thread_ = ApplicationThread::create<InboundTestThread>(logger_->logger, *reactor_);
        reactor_->register_thread(stub_thread_);

        allocator_ = std::make_unique<ExpandableSlabAllocator>(4 * 1024 * 1024);
    }

    void TearDown() override {
        if (client_fd_ != -1) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
        allocator_.reset();
        stub_thread_.reset();
        reactor_.reset();
        logger_.reset();
    }

    // Builds a RawBytes listener bound to an ephemeral loopback port, connects a
    // client to it, and hands the accepted connection to the manager. The
    // listener is owned by the test rather than by the manager, which is what
    // lets on_accept be called without running the event loop.
    bool accept_one_connection(ConnectionID id) {
        listener_.configuration.address = NetworkEndpointConfiguration{"127.0.0.1", 0};
        listener_.configuration.target_thread_id = ThreadID{1};
        listener_.configuration.protocol_type = ProtocolType{ProtocolType::RawBytes};
        listener_.configuration.raw_buffer_capacity = 1024 * 1024;

        auto [local_address, address_error] = InetAddress::create("127.0.0.1", 0);
        if (!local_address) {
            return false;
        }
        auto [acceptor, acceptor_error] = TcpAcceptor::create(*local_address, 8);
        if (!acceptor) {
            return false;
        }
        listener_.acceptor = std::move(acceptor);

        // The port was chosen by the kernel, and the acceptor reports only its
        // descriptor, so ask the socket.
        sockaddr_in bound{};
        socklen_t bound_length = sizeof(bound);
        if (::getsockname(listener_.acceptor->get_listening_file_descriptor(), reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
            return false;
        }
        const uint16_t port = ntohs(bound.sin_port);
        client_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client_fd_ == -1) {
            return false;
        }
        // A small receive buffer on the far end keeps the sender blocked once
        // the window closes, which is what leaves a send pending.
        const int tiny = 4096;
        ::setsockopt(client_fd_, SOL_SOCKET, SO_RCVBUF, &tiny, sizeof(tiny));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        if (::connect(client_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }

        reactor_->inbound_manager().on_accept(listener_, id);
        return true;
    }

    // A raw send command carrying a chunk of the given size. raw_chunk_ptr_ is
    // set and pdu_chunk_ptr_ is left null, exactly as the gateway's send_raw
    // path builds it.
    ReactorControlCommand make_raw_send(ConnectionID id, uint32_t byte_count) {
        auto [slab_id, chunk] = allocator_->allocate(byte_count);
        std::memset(chunk, 0, byte_count);

        ReactorControlCommand command(ReactorControlCommand::CommandTag::SendRaw);
        command.connection_id_ = id;
        command.allocator_ = allocator_.get();
        command.slab_id_ = slab_id;
        command.raw_chunk_ptr_ = chunk;
        command.raw_byte_count_ = byte_count;
        return command;
    }

    ServiceRegistry registry_;
    std::unique_ptr<LoggerWithSink> logger_;
    std::unique_ptr<Reactor> reactor_;
    std::shared_ptr<ApplicationThread> stub_thread_;
    std::unique_ptr<ExpandableSlabAllocator> allocator_;
    InboundListener listener_;
    int client_fd_{-1};
};

TEST_F(InboundConnectionManagerTest, TeardownFreesAStashedRawSendRatherThanANullPduPointer) {
    const ConnectionID conn_id{8};
    ASSERT_TRUE(accept_one_connection(conn_id)) << "Failed to accept a loopback connection";

    InboundConnectionManager& manager = reactor_->inbound_manager();

    // Fill the socket until a send cannot complete, then send once more. The
    // second command is stashed in pending_send_ rather than written.
    constexpr uint32_t chunk_bytes = 256 * 1024;
    for (int attempt = 0; attempt < 8 && !manager.is_send_blocked(); ++attempt) {
        const ReactorControlCommand command = make_raw_send(conn_id, chunk_bytes);
        ASSERT_TRUE(manager.process_send_raw_command(command)) << "process_send_raw_command did not find connection " << conn_id.get_value();
    }

    if (!manager.is_send_blocked()) {
        // The kernel took everything offered. Nothing is stashed, so the
        // condition this test is about does not exist on this machine.
        manager.teardown_connection(conn_id, "cleanup", DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
        GTEST_SKIP() << "Socket buffers absorbed every send -- no command was stashed";
    }

    const int slabs_before = allocator_->slab_count();

    // The peer resets the connection while the send is stashed. Teardown must
    // free the chunk the stashed command actually carries. Reading the PDU
    // field of a SendRaw command yields nullptr, and deallocate refuses that:
    // the exception escapes the reactor's event loop and stops the process.
    EXPECT_NO_THROW(manager.teardown_connection(conn_id, "socket error on inbound connection: Connection reset by peer",
                                                DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent}));

    // And the chunk really was returned: allocating the same size again must
    // not need a new slab.
    auto [slab_id, chunk] = allocator_->allocate(chunk_bytes);
    EXPECT_NE(chunk, nullptr);
    EXPECT_LE(allocator_->slab_count(), slabs_before) << "Slab count grew after teardown -- the stashed chunk was not freed";
    allocator_->deallocate(slab_id, chunk);
}

TEST_F(InboundConnectionManagerTest, PausingAndResumingReadingSetTheConnectionsFlag) {
    const ConnectionID id{7};
    ASSERT_TRUE(accept_one_connection(id));
    InboundConnection* conn = reactor_->inbound_manager().find_by_id(id);
    ASSERT_NE(conn, nullptr);
    EXPECT_FALSE(conn->reading_paused_by_application());
    EXPECT_TRUE(reactor_->inbound_manager().pause_reading(id));
    EXPECT_TRUE(conn->reading_paused_by_application());
    EXPECT_TRUE(reactor_->inbound_manager().resume_reading(id));
    EXPECT_FALSE(conn->reading_paused_by_application());
}

TEST_F(InboundConnectionManagerTest, PausingOrResumingAConnectionThisManagerDoesNotHoldReportsFalse) {
    EXPECT_FALSE(reactor_->inbound_manager().pause_reading(ConnectionID{99}));
    EXPECT_FALSE(reactor_->inbound_manager().resume_reading(ConnectionID{99}));
}

TEST_F(InboundConnectionManagerTest, ACommitOfBytesDoesNotUndoAnApplicationsPause) {
    // A raw-bytes handler resumes its own reading when the application commits bytes; that path must
    // leave the application's own pause in place.
    const ConnectionID id{8};
    ASSERT_TRUE(accept_one_connection(id));
    ASSERT_TRUE(reactor_->inbound_manager().pause_reading(id));
    EXPECT_TRUE(reactor_->inbound_manager().process_commit_raw_bytes(id, 0));
    InboundConnection* conn = reactor_->inbound_manager().find_by_id(id);
    ASSERT_NE(conn, nullptr);
    EXPECT_TRUE(conn->reading_paused_by_application());
}

TEST(ReactorControlCommandTest, TheNewCommandsNameThemselves) {
    EXPECT_EQ(ReactorControlCommand(ReactorControlCommand::CommandTag::PauseReading).as_string(), "PauseReading");
    EXPECT_EQ(ReactorControlCommand(ReactorControlCommand::CommandTag::ResumeReading).as_string(), "ResumeReading");
}

} // namespaces
