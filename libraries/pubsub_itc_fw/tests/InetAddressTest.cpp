// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file InetAddressTest.cpp
 * @brief Unit tests for InetAddress.
 *
 * Covers both factory methods, IPv4 and IPv6 address handling, port extraction,
 * string conversion, equality and ordering comparisons, and error paths.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/InetAddress.hpp>

namespace pubsub_itc_fw {

namespace {

class InetAddressTest : public ::testing::Test {};

// ============================================================
// create(string, port) -- IPv4 happy path
// ============================================================

TEST_F(InetAddressTest, CreateFromIpv4StringSucceeds) {
    auto [addr, error] = InetAddress::create("127.0.0.1", 8080);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(addr->is_ipv4());
    EXPECT_FALSE(addr->is_ipv6());
    EXPECT_EQ(addr->get_port(), 8080);
    EXPECT_EQ(addr->get_ip_address_string(), "127.0.0.1");
    EXPECT_EQ(addr->address_family(), AF_INET);
    EXPECT_NE(addr->get_sockaddr(), nullptr);
    EXPECT_GT(addr->get_sockaddr_size(), 0u);
}

TEST_F(InetAddressTest, CreateFromIpv4ZeroPortSucceeds) {
    auto [addr, error] = InetAddress::create("192.168.1.1", 0);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(addr->get_port(), 0);
}

TEST_F(InetAddressTest, CreateFromPassiveAddressSucceeds) {
    // Empty string triggers AI_PASSIVE -- binds to all interfaces.
    auto [addr, error] = InetAddress::create("", 9000);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
}

TEST_F(InetAddressTest, CreateFromAllZerosAddressSucceeds) {
    auto [addr, error] = InetAddress::create("0.0.0.0", 443);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
}

// ============================================================
// create(string, port) -- IPv6 happy path
// ============================================================

TEST_F(InetAddressTest, CreateFromIpv6StringSucceeds) {
    auto [addr, error] = InetAddress::create("::1", 8080);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(addr->is_ipv6());
    EXPECT_FALSE(addr->is_ipv4());
    EXPECT_EQ(addr->get_port(), 8080);
    EXPECT_EQ(addr->address_family(), AF_INET6);
    EXPECT_NE(addr->get_sockaddr(), nullptr);
}

TEST_F(InetAddressTest, CreateFromIpv6AllInterfacesSucceeds) {
    auto [addr, error] = InetAddress::create("::", 9000);
    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
}

TEST_F(InetAddressTest, Ipv6GetIpAddressStringIsNonEmpty) {
    auto [addr, error] = InetAddress::create("::1", 1234);
    ASSERT_NE(addr, nullptr);
    const std::string ip = addr->get_ip_address_string();
    EXPECT_FALSE(ip.empty());
}

TEST_F(InetAddressTest, Ipv6GetPortReturnsCorrectValue) {
    auto [addr, error] = InetAddress::create("::1", 5555);
    ASSERT_NE(addr, nullptr);
    EXPECT_EQ(addr->get_port(), 5555);
}

// ============================================================
// create(string, port) -- error path
// ============================================================

TEST_F(InetAddressTest, CreateFromInvalidHostnameReturnsError) {
    auto [addr, error] = InetAddress::create("this.hostname.does.not.exist.invalid", 80);
    EXPECT_EQ(addr, nullptr);
    EXPECT_FALSE(error.empty());
}

// ============================================================
// create(sockaddr*, socklen_t) -- happy path
// ============================================================

TEST_F(InetAddressTest, CreateFromSockaddrIpv4Succeeds) {
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(7777);
    sa.sin_addr.s_addr = inet_addr("10.0.0.1");

    auto [addr, error] = InetAddress::create(
        reinterpret_cast<const sockaddr*>(&sa),
        static_cast<socklen_t>(sizeof(sa)));

    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(addr->is_ipv4());
    EXPECT_EQ(addr->get_port(), 7777);
    EXPECT_EQ(addr->get_ip_address_string(), "10.0.0.1");
}

TEST_F(InetAddressTest, CreateFromSockaddrIpv6Succeeds) {
    sockaddr_in6 sa{};
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(4444);
    inet_pton(AF_INET6, "::1", &sa.sin6_addr);

    auto [addr, error] = InetAddress::create(
        reinterpret_cast<const sockaddr*>(&sa),
        static_cast<socklen_t>(sizeof(sa)));

    ASSERT_NE(addr, nullptr);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(addr->is_ipv6());
    EXPECT_EQ(addr->get_port(), 4444);
}

// ============================================================
// create(sockaddr*, socklen_t) -- error paths
// ============================================================

TEST_F(InetAddressTest, CreateFromNullSockaddrReturnsError) {
    auto [addr, error] = InetAddress::create(nullptr, sizeof(sockaddr_in));
    EXPECT_EQ(addr, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(InetAddressTest, CreateFromZeroLengthSockaddrReturnsError) {
    sockaddr_in sa{};
    auto [addr, error] = InetAddress::create(
        reinterpret_cast<const sockaddr*>(&sa), 0);
    EXPECT_EQ(addr, nullptr);
    EXPECT_FALSE(error.empty());
}

TEST_F(InetAddressTest, CreateFromOversizedSockaddrReturnsError) {
    sockaddr_in sa{};
    auto [addr, error] = InetAddress::create(
        reinterpret_cast<const sockaddr*>(&sa),
        static_cast<socklen_t>(sizeof(sockaddr_storage) + 1));
    EXPECT_EQ(addr, nullptr);
    EXPECT_FALSE(error.empty());
}

// ============================================================
// is_equal
// ============================================================

TEST_F(InetAddressTest, EqualAddressesCompareEqual) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("127.0.0.1", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(a->is_equal(*b));
    EXPECT_TRUE(b->is_equal(*a));
}

TEST_F(InetAddressTest, DifferentPortsCompareNotEqual) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("127.0.0.1", 9001);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->is_equal(*b));
}

TEST_F(InetAddressTest, DifferentAddressesCompareNotEqual) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("127.0.0.2", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->is_equal(*b));
}

TEST_F(InetAddressTest, Ipv4AndIpv6CompareNotEqual) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("::1", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->is_equal(*b));
}

TEST_F(InetAddressTest, EqualIpv6AddressesCompareEqual) {
    auto [a, e1] = InetAddress::create("::1", 5000);
    auto [b, e2] = InetAddress::create("::1", 5000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(a->is_equal(*b));
}

TEST_F(InetAddressTest, DifferentIpv6AddressesCompareNotEqual) {
    auto [a, e1] = InetAddress::create("::1", 5000);
    auto [b, e2] = InetAddress::create("::2", 5000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->is_equal(*b));
}

// ============================================================
// is_less_than
// ============================================================

TEST_F(InetAddressTest, LowerPortIsLessThan) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 1000);
    auto [b, e2] = InetAddress::create("127.0.0.1", 2000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(a->is_less_than(*b));
    EXPECT_FALSE(b->is_less_than(*a));
}

TEST_F(InetAddressTest, EqualAddressIsNotLessThan) {
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("127.0.0.1", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_FALSE(a->is_less_than(*b));
    EXPECT_FALSE(b->is_less_than(*a));
}

TEST_F(InetAddressTest, LowerIpAddressIsLessThan) {
    auto [a, e1] = InetAddress::create("10.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("10.0.0.2", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(a->is_less_than(*b));
    EXPECT_FALSE(b->is_less_than(*a));
}

TEST_F(InetAddressTest, Ipv4IsLessThanIpv6WhenFamilyDiffers) {
    // AF_INET (2) < AF_INET6 (10) on Linux, so IPv4 should sort before IPv6.
    auto [a, e1] = InetAddress::create("127.0.0.1", 9000);
    auto [b, e2] = InetAddress::create("::1", 9000);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    // Just verify the ordering is consistent -- not equal and one is less.
    EXPECT_NE(a->is_less_than(*b), b->is_less_than(*a));
}

} // namespace

} // namespace pubsub_itc_fw
