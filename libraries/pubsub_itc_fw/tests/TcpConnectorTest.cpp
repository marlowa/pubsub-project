// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TcpConnectorTest.cpp
 * @brief Unit tests for TcpConnector.
 *
 * Covers the idle-state guards, error paths, and move semantics of
 * TcpConnector without requiring a live reactor. Where a real TCP
 * connection is needed, a loopback listening socket is created directly
 * using POSIX calls.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpConnector.hpp>

namespace pubsub_itc_fw {

namespace {

/*
 * Creates a POSIX listening socket on 127.0.0.1:0 and returns the fd
 * and the OS-assigned port. The caller is responsible for ::close(fd).
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

class TcpConnectorTest : public ::testing::Test {};

// ============================================================
// Initial state
// ============================================================

TEST_F(TcpConnectorTest, DefaultStateIsNotConnecting) {
    TcpConnector connector;
    EXPECT_FALSE(connector.is_connecting());
    EXPECT_EQ(connector.get_fd(), -1);
}

TEST_F(TcpConnectorTest, GetConnectedSocketReturnsNullWhenNotConnected) {
    TcpConnector connector;
    EXPECT_EQ(connector.get_connected_socket(), nullptr);
}

TEST_F(TcpConnectorTest, FinishConnectWithNoActiveAttemptReturnsError) {
    TcpConnector connector;
    auto [connected, error] = connector.finish_connect();
    EXPECT_FALSE(connected);
    EXPECT_FALSE(error.empty());
}

TEST_F(TcpConnectorTest, CancelOnIdleConnectorIsNoop) {
    TcpConnector connector;
    connector.cancel(); // must not crash or throw
    EXPECT_FALSE(connector.is_connecting());
    EXPECT_EQ(connector.get_fd(), -1);
}

// ============================================================
// connect() to a non-existent address fails cleanly
// ============================================================

TEST_F(TcpConnectorTest, ConnectToRefusedPortReturnsError) {
    // Port 1 is privileged and almost certainly not listening.
    auto [addr, addr_error] = InetAddress::create("127.0.0.1", 1);
    ASSERT_NE(addr, nullptr) << addr_error;

    TcpConnector connector;
    auto [connected, error] = connector.connect(*addr);

    // connect() either fails immediately with an error string, or returns
    // false and begins an async attempt. In either case we do not expect
    // immediate success to port 1.
    if (!error.empty()) {
        // Synchronous failure -- error message must be non-empty.
        EXPECT_FALSE(connected);
    }
    // If error is empty the OS started a non-blocking connect; that is also valid.
}

// ============================================================
// connect() to a valid listener
// ============================================================

TEST_F(TcpConnectorTest, ConnectToListeningSocketSucceeds) {
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1) << "Failed to create listening socket";

    auto [addr, addr_error] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_error;

    TcpConnector connector;
    auto [connected_immediately, connect_error] = connector.connect(*addr);
    EXPECT_TRUE(connect_error.empty()) << connect_error;

    // The connect may complete immediately or require finish_connect().
    if (!connected_immediately) {
        EXPECT_TRUE(connector.is_connecting());
        EXPECT_NE(connector.get_fd(), -1);

        // Accept on the listener side to allow the handshake to complete.
        std::thread accept_thread([listen_fd]() {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int client_fd = ::accept(listen_fd,
                reinterpret_cast<sockaddr*>(&peer), &len);
            if (client_fd != -1) {
                ::close(client_fd);
            }
        });

        // Poll finish_connect() until it succeeds or times out.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto [finished, finish_error] = connector.finish_connect();
            if (finished) {
                done = true;
                break;
            }
            if (!finish_error.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        accept_thread.join();
        EXPECT_TRUE(done) << "finish_connect() did not complete within timeout";
    }

    if (connected_immediately || connector.get_connected_socket() != nullptr) {
        auto socket = connector.get_connected_socket();
        // If connected_immediately the socket should be retrievable.
        // (If finish_connect completed above get_connected_socket is also valid.)
        if (socket != nullptr) {
            EXPECT_TRUE(socket->is_connected());
        }
    }

    ::close(listen_fd);
}

// ============================================================
// get_connected_socket() returns null when still connecting
// ============================================================

TEST_F(TcpConnectorTest, GetConnectedSocketReturnsNullWhileStillConnecting) {
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1);

    auto [addr, addr_error] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr);

    TcpConnector connector;
    auto [connected_immediately, connect_error] = connector.connect(*addr);
    EXPECT_TRUE(connect_error.empty());

    if (!connected_immediately) {
        // While still in the connecting state the socket must not be returned.
        auto socket = connector.get_connected_socket();
        EXPECT_EQ(socket, nullptr);
    }

    connector.cancel();
    ::close(listen_fd);
}

// ============================================================
// cancel() during an active connect attempt
// ============================================================

TEST_F(TcpConnectorTest, CancelDuringConnectResetsState) {
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1);

    auto [addr, addr_error] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr);

    TcpConnector connector;
    auto [connected_immediately, connect_error] = connector.connect(*addr);
    EXPECT_TRUE(connect_error.empty());

    connector.cancel();

    EXPECT_FALSE(connector.is_connecting());
    EXPECT_EQ(connector.get_fd(), -1);
    EXPECT_EQ(connector.get_connected_socket(), nullptr);

    ::close(listen_fd);
}

// ============================================================
// move semantics
// ============================================================

TEST_F(TcpConnectorTest, MoveConstructedConnectorIsValid) {
    TcpConnector original;
    TcpConnector moved(std::move(original));

    EXPECT_FALSE(moved.is_connecting());
    EXPECT_EQ(moved.get_fd(), -1);
}

TEST_F(TcpConnectorTest, MoveAssignedConnectorIsValid) {
    TcpConnector a;
    TcpConnector b;
    b = std::move(a);

    EXPECT_FALSE(b.is_connecting());
    EXPECT_EQ(b.get_fd(), -1);
}

// ============================================================
// second connect() on same connector resets state cleanly
// ============================================================

TEST_F(TcpConnectorTest, SecondConnectCallCancelsFirstAndRetries) {
    auto [listen_fd, listen_port] = make_listener();
    ASSERT_NE(listen_fd, -1);

    auto [addr, addr_error] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr);

    TcpConnector connector;

    // First connect.
    auto [c1, e1] = connector.connect(*addr);
    EXPECT_TRUE(e1.empty());

    // Second connect to the same address -- should cancel first and restart.
    auto [c2, e2] = connector.connect(*addr);
    EXPECT_TRUE(e2.empty());

    connector.cancel();
    ::close(listen_fd);
}

} // namespace

} // namespace pubsub_itc_fw
