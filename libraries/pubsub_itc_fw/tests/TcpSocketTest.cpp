// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TcpSocketTest.cpp
 * @brief Unit tests for TcpSocket.
 *
 * These tests exercise the uncovered functions and paths in TcpSocket.cpp:
 *
 *   GetPeerAddressIPv4
 *     Creates a connected socketpair, calls get_peer_address() and verifies
 *     a valid address is returned.
 *
 *   GetLocalAddressIPv4
 *     Creates a bound listening socket, calls get_local_address() and
 *     verifies the returned address matches the bound port.
 *
 *   GetPeerAddressIPv6
 *     Connects to a loopback IPv6 listener, calls get_peer_address() and
 *     verifies a valid IPv6 address is returned.
 *
 *   GetLocalAddressIPv6
 *     Creates a bound IPv6 listening socket, calls get_local_address() and
 *     verifies the returned address is IPv6.
 *
 *   ShutdownHalfClose
 *     Calls shutdown(SHUT_WR) on a connected socket and verifies success.
 *
 *   ShutdownBothDirections
 *     Calls shutdown(SHUT_RDWR) on a connected socket and verifies success.
 *
 *   SendOnClosedSocket
 *     Closes a socket then calls send() on it, verifying the error path.
 *
 *   SendEmptyData
 *     Calls send() with an empty span, verifying the fast-return path.
 *
 *   ReceiveOnClosedSocket
 *     Closes a socket then calls receive() on it, verifying the error path.
 *
 *   ReceiveEmptyBuffer
 *     Calls receive() with an empty span, verifying the fast-return path.
 *
 *   AdoptInvalidFd
 *     Calls TcpSocket::adopt(-1) and verifies it returns nullptr with an error.
 *
 *   GetPeerAddressOnClosedSocket
 *     Closes a socket and verifies get_peer_address() returns nullptr with error.
 *
 *   GetLocalAddressOnClosedSocket
 *     Closes a socket and verifies get_local_address() returns nullptr with error.
 *
 *   ShutdownOnClosedSocket
 *     Closes a socket and verifies shutdown() returns false with an error message.
 *
 *   ConnectImmediateLoopback
 *     Connects to a listening loopback socket -- on Linux this often completes
 *     immediately (EISCONN or immediate success), exercising the non-EINPROGRESS
 *     connect path.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/utils/SimpleSpan.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Helpers
// ============================================================

/*
 * Creates a listening TCP socket on 127.0.0.1:0.
 * Returns {fd, port}. Caller owns fd and must ::close() it.
 */
static std::pair<int, uint16_t> make_ipv4_listener() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) return {-1, 0};
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return {-1, 0};
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd); return {-1, 0};
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    return {fd, ntohs(bound.sin_port)};
}

/*
 * Creates a listening TCP socket on [::1]:0 (IPv6 loopback).
 * Returns {fd, port}. Caller owns fd and must ::close() it.
 * Returns {-1, 0} if IPv6 is not available.
 */
static std::pair<int, uint16_t> make_ipv6_listener() {
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd == -1) return {-1, 0};
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Restrict to IPv6 only so we get a pure IPv6 address back.
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = 0;
    addr.sin6_addr   = in6addr_loopback;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return {-1, 0};
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd); return {-1, 0};
    }
    sockaddr_in6 bound{};
    socklen_t len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    return {fd, ntohs(bound.sin6_port)};
}

// ============================================================
// Test fixture
// ============================================================
class TcpSocketTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================
// Test: get_peer_address() on a connected IPv4 socket
// ============================================================
TEST_F(TcpSocketTest, GetPeerAddressIPv4) {
    auto [listen_fd, listen_port] = make_ipv4_listener();
    ASSERT_NE(listen_fd, -1);

    auto [client_socket, create_err] = TcpSocket::create(AF_INET);
    ASSERT_NE(client_socket, nullptr) << create_err;

    auto [addr, addr_err] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_err;

    auto [connected, connect_err] = client_socket->connect(*addr);
    // EINPROGRESS is normal for non-blocking connect -- not an error.
    EXPECT_TRUE(connect_err.empty()) << "connect() error: " << connect_err;

    // Accept the connection on the server side.
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int accepted_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    ASSERT_NE(accepted_fd, -1);

    auto [peer_addr, peer_err] = client_socket->get_peer_address();
    EXPECT_NE(peer_addr, nullptr) << "get_peer_address() returned nullptr: " << peer_err;
    if (peer_addr) {
        EXPECT_TRUE(peer_addr->is_ipv4());
        EXPECT_EQ(peer_addr->get_port(), listen_port);
    }

    ::close(accepted_fd);
    ::close(listen_fd);
}

// ============================================================
// Test: get_local_address() on a bound socket
// ============================================================
TEST_F(TcpSocketTest, GetLocalAddressIPv4) {
    auto [listen_fd, listen_port] = make_ipv4_listener();
    ASSERT_NE(listen_fd, -1);

    auto [socket, err] = TcpSocket::adopt(listen_fd);
    ASSERT_NE(socket, nullptr) << err;

    auto [local_addr, local_err] = socket->get_local_address();
    EXPECT_NE(local_addr, nullptr) << "get_local_address() returned nullptr: " << local_err;
    if (local_addr) {
        EXPECT_TRUE(local_addr->is_ipv4());
        EXPECT_EQ(local_addr->get_port(), listen_port);
    }
    // socket owns listen_fd now, no need to close separately
}

// ============================================================
// Test: get_peer_address() on a connected IPv6 socket
// ============================================================
TEST_F(TcpSocketTest, GetPeerAddressIPv6) {
    auto [listen_fd, listen_port] = make_ipv6_listener();
    if (listen_fd == -1) {
        GTEST_SKIP() << "IPv6 not available on this host";
    }

    auto [client_socket, create_err] = TcpSocket::create(AF_INET6);
    ASSERT_NE(client_socket, nullptr) << create_err;

    auto [addr, addr_err] = InetAddress::create("::1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_err;

    auto [connected, connect_err] = client_socket->connect(*addr);
    EXPECT_TRUE(connect_err.empty()) << "connect() error: " << connect_err;

    sockaddr_in6 peer{};
    socklen_t len = sizeof(peer);
    const int accepted_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    ASSERT_NE(accepted_fd, -1);

    auto [peer_addr, peer_err] = client_socket->get_peer_address();
    EXPECT_NE(peer_addr, nullptr) << "get_peer_address() returned nullptr: " << peer_err;
    if (peer_addr) {
        EXPECT_TRUE(peer_addr->is_ipv6());
        EXPECT_EQ(peer_addr->get_port(), listen_port);
    }

    ::close(accepted_fd);
    ::close(listen_fd);
}

// ============================================================
// Test: get_local_address() on a bound IPv6 socket
// ============================================================
TEST_F(TcpSocketTest, GetLocalAddressIPv6) {
    auto [listen_fd, listen_port] = make_ipv6_listener();
    if (listen_fd == -1) {
        GTEST_SKIP() << "IPv6 not available on this host";
    }

    auto [socket, err] = TcpSocket::adopt(listen_fd);
    ASSERT_NE(socket, nullptr) << err;

    auto [local_addr, local_err] = socket->get_local_address();
    EXPECT_NE(local_addr, nullptr) << "get_local_address() returned nullptr: " << local_err;
    if (local_addr) {
        EXPECT_TRUE(local_addr->is_ipv6());
        EXPECT_EQ(local_addr->get_port(), listen_port);
    }
}

// ============================================================
// Test: shutdown(SHUT_WR) on a connected socket
// ============================================================
TEST_F(TcpSocketTest, ShutdownHalfClose) {
    auto [listen_fd, listen_port] = make_ipv4_listener();
    ASSERT_NE(listen_fd, -1);

    auto [client_socket, create_err] = TcpSocket::create(AF_INET);
    ASSERT_NE(client_socket, nullptr) << create_err;

    auto [addr, addr_err] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_err;
    auto [connected, connect_err] = client_socket->connect(*addr);
    EXPECT_TRUE(connect_err.empty()) << "connect() error: " << connect_err;

    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int accepted_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    ASSERT_NE(accepted_fd, -1);

    auto [ok, err] = client_socket->shutdown(SHUT_WR);
    EXPECT_TRUE(ok) << "shutdown(SHUT_WR) failed: " << err;

    ::close(accepted_fd);
    ::close(listen_fd);
}

// ============================================================
// Test: shutdown(SHUT_RDWR) on a connected socket
// ============================================================
TEST_F(TcpSocketTest, ShutdownBothDirections) {
    auto [listen_fd, listen_port] = make_ipv4_listener();
    ASSERT_NE(listen_fd, -1);

    auto [client_socket, create_err] = TcpSocket::create(AF_INET);
    ASSERT_NE(client_socket, nullptr) << create_err;

    auto [addr, addr_err] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_err;
    auto [connected, connect_err] = client_socket->connect(*addr);
    EXPECT_TRUE(connect_err.empty()) << "connect() error: " << connect_err;

    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int accepted_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    ASSERT_NE(accepted_fd, -1);

    auto [ok, err] = client_socket->shutdown(SHUT_RDWR);
    EXPECT_TRUE(ok) << "shutdown(SHUT_RDWR) failed: " << err;

    ::close(accepted_fd);
    ::close(listen_fd);
}

// ============================================================
// Test: shutdown() on a closed socket returns false with error
// ============================================================
TEST_F(TcpSocketTest, ShutdownOnClosedSocket) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;
    socket->close();

    auto [ok, shutdown_err] = socket->shutdown(SHUT_RDWR);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(shutdown_err.empty());
}

// ============================================================
// Test: send() on a closed socket returns error
// ============================================================
TEST_F(TcpSocketTest, SendOnClosedSocket) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;
    socket->close();

    const uint8_t data[] = {1, 2, 3};
    auto [bytes_sent, send_err] = socket->send(utils::SimpleSpan<const uint8_t>{data});
    EXPECT_LT(bytes_sent, 0);
    EXPECT_FALSE(send_err.empty());
}

// ============================================================
// Test: send() with empty span returns 0 immediately
// ============================================================
TEST_F(TcpSocketTest, SendEmptyData) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;

    utils::SimpleSpan<const uint8_t> empty_span{};
    auto [bytes_sent, send_err] = socket->send(empty_span);
    EXPECT_EQ(bytes_sent, 0);
    EXPECT_TRUE(send_err.empty());
}

// ============================================================
// Test: receive() on a closed socket returns error
// ============================================================
TEST_F(TcpSocketTest, ReceiveOnClosedSocket) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;
    socket->close();

    uint8_t buf[64];
    auto [bytes_recv, recv_err] = socket->receive(utils::SimpleSpan<uint8_t>{buf});
    EXPECT_LT(bytes_recv, 0);
    EXPECT_FALSE(recv_err.empty());
}

// ============================================================
// Test: receive() with empty buffer returns 0 immediately
// ============================================================
TEST_F(TcpSocketTest, ReceiveEmptyBuffer) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;

    utils::SimpleSpan<uint8_t> empty_span{};
    auto [bytes_recv, recv_err] = socket->receive(empty_span);
    EXPECT_EQ(bytes_recv, 0);
    EXPECT_TRUE(recv_err.empty());
}

// ============================================================
// Test: adopt(-1) returns nullptr with error
// ============================================================
TEST_F(TcpSocketTest, AdoptInvalidFd) {
    auto [socket, err] = TcpSocket::adopt(-1);
    EXPECT_EQ(socket, nullptr);
    EXPECT_FALSE(err.empty());
}

// ============================================================
// Test: get_peer_address() on a closed socket returns nullptr
// ============================================================
TEST_F(TcpSocketTest, GetPeerAddressOnClosedSocket) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;
    socket->close();

    auto [peer_addr, peer_err] = socket->get_peer_address();
    EXPECT_EQ(peer_addr, nullptr);
    EXPECT_FALSE(peer_err.empty());
}

// ============================================================
// Test: get_local_address() on a closed socket returns nullptr
// ============================================================
TEST_F(TcpSocketTest, GetLocalAddressOnClosedSocket) {
    auto [socket, err] = TcpSocket::create(AF_INET);
    ASSERT_NE(socket, nullptr) << err;
    socket->close();

    auto [local_addr, local_err] = socket->get_local_address();
    EXPECT_EQ(local_addr, nullptr);
    EXPECT_FALSE(local_err.empty());
}

// ============================================================
// Test: connect to a listening loopback socket completes
// (exercises non-EINPROGRESS path on Linux loopback)
// ============================================================
TEST_F(TcpSocketTest, ConnectImmediateLoopback) {
    auto [listen_fd, listen_port] = make_ipv4_listener();
    ASSERT_NE(listen_fd, -1);

    auto [client_socket, create_err] = TcpSocket::create(AF_INET);
    ASSERT_NE(client_socket, nullptr) << create_err;

    auto [addr, addr_err] = InetAddress::create("127.0.0.1", listen_port);
    ASSERT_NE(addr, nullptr) << addr_err;

    // On Linux loopback, connect() may return EINPROGRESS or succeed immediately.
    // Either way is a valid result -- we just verify it does not crash or throw.
    auto [connected, connect_err] = client_socket->connect(*addr);
    // connected may be true (immediate) or false (EINPROGRESS) -- both are valid.
    // If it failed with an error message, that is a test environment issue.
    if (!connected) {
        // EINPROGRESS is expected -- not an error
        EXPECT_TRUE(connect_err.empty())
            << "connect() failed unexpectedly: " << connect_err;
    }

    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int accepted_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
    // Accept may fail if the connect was still in progress and we did not wait.
    // That is acceptable for this test.
    if (accepted_fd != -1) {
        ::close(accepted_fd);
    }

    ::close(listen_fd);
}

} // namespace pubsub_itc_fw::tests
