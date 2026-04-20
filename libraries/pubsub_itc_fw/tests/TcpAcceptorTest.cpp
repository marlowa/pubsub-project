// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TcpAcceptorTest.cpp
 * @brief Unit tests for TcpAcceptor.
 *
 * Tests:
 *
 *   MoveConstructorTransfersOwnership
 *     Creates a TcpAcceptor via TcpAcceptor::create(), then move-constructs a
 *     second TcpAcceptor from it. Verifies the new acceptor has a valid fd and
 *     the original is left in a valid but empty state (fd == -1). Covers the
 *     move constructor (TcpAcceptor(TcpAcceptor&&)).
 *
 *   MoveAssignmentTransfersOwnership
 *     Creates two TcpAcceptors via TcpAcceptor::create(), then move-assigns the
 *     first into the second. Verifies the destination has the source's fd and
 *     the source is left empty. Covers operator=(TcpAcceptor&&).
 */

#include <netinet/in.h>

#include <memory>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>

namespace pubsub_itc_fw::tests {

// Creates a TcpAcceptor bound to 127.0.0.1:0. Asserts on failure.
static std::unique_ptr<TcpAcceptor> make_acceptor() {
    auto [addr, addr_err] = InetAddress::create("127.0.0.1", 0);
    if (!addr) {
        ADD_FAILURE() << "InetAddress::create failed: " << addr_err;
        return nullptr;
    }
    auto [acceptor, err] = TcpAcceptor::create(*addr, 4);
    if (!acceptor) {
        ADD_FAILURE() << "TcpAcceptor::create failed: " << err;
        return nullptr;
    }
    return std::move(acceptor);
}

// ============================================================
// Test: move constructor transfers p_impl_ ownership.
// ============================================================
TEST(TcpAcceptorTest, MoveConstructorTransfersOwnership) {
    auto original = make_acceptor();
    ASSERT_NE(original, nullptr);

    const int original_fd = original->get_listening_file_descriptor();
    ASSERT_NE(original_fd, -1);

    // Move-construct.
    TcpAcceptor moved(std::move(*original));

    // The moved-into acceptor owns the fd.
    EXPECT_EQ(moved.get_listening_file_descriptor(), original_fd);

    // The moved-from acceptor has an empty p_impl_ — fd should be -1.
    EXPECT_EQ(original->get_listening_file_descriptor(), -1);
}

// ============================================================
// Test: move assignment transfers p_impl_ ownership.
// ============================================================
TEST(TcpAcceptorTest, MoveAssignmentTransfersOwnership) {
    auto source = make_acceptor();
    ASSERT_NE(source, nullptr);

    auto dest = make_acceptor();
    ASSERT_NE(dest, nullptr);

    const int source_fd = source->get_listening_file_descriptor();
    ASSERT_NE(source_fd, -1);

    // Move-assign source into dest.
    *dest = std::move(*source);

    // dest now owns the source's fd.
    EXPECT_EQ(dest->get_listening_file_descriptor(), source_fd);

    // source is left empty.
    EXPECT_EQ(source->get_listening_file_descriptor(), -1);
}

} // namespace pubsub_itc_fw::tests
