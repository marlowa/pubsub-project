// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <OrderKey.hpp>

#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "SessionIdentity.hpp"

// The order book's key decides which orders are the same order. These tests pin what it
// must distinguish and -- just as importantly -- what it must NOT distinguish, which is
// where the key has been wrong twice.
//
// It was first keyed on the connection id alone, so two gateways shared one session-id
// space and a client on one could collide with, or cancel, an order belonging to a client
// on the other. The instance axis was then missing in exactly the same way once a protocol
// ran as more than one process.
//
// Both fixes added axes. The third change removed them: keying on a connection at all was
// the underlying mistake, because a connection is where a member happens to be rather than
// who it is. An order placed on a connection that has since dropped was filed under a key
// nothing could reproduce, so the member could see it resting but never cancel it, and
// after a gateway failover the instance in the key named a dead process. The key is now the
// session identity -- comp id and protocol -- and the tests below say what that buys and
// what it must still keep apart.

using fix_common::SessionIdentity;
using matching_engine::OrderKey;
using matching_engine::OrderKeyHash;

namespace {

using OrderBook = std::unordered_map<OrderKey, std::string, OrderKeyHash>;

SessionIdentity fix_session(std::string_view comp_id) {
    return SessionIdentity::make(comp_id, gateway_ids::fix_order_gateway);
}

SessionIdentity binary_session(std::string_view comp_id) {
    return SessionIdentity::make(comp_id, gateway_ids::binary_order_gateway);
}

TEST(OrderKeyTest, SameSessionAndClOrdIdIsTheSameOrder) {
    const OrderKey first = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    const OrderKey second = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");

    EXPECT_TRUE(first == second);
    EXPECT_EQ(OrderKeyHash{}(first), OrderKeyHash{}(second));
}

TEST(OrderKeyTest, DifferentClOrdIdOnOneSessionIsADifferentOrder) {
    const OrderKey first = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    const OrderKey second = OrderKey::make(fix_session("MEMBER-A"), "ORDER-2");

    EXPECT_FALSE(first == second);
}

TEST(OrderKeyTest, DifferentCompIdOnOneProtocolIsADifferentOrder) {
    const OrderKey first = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    const OrderKey second = OrderKey::make(fix_session("MEMBER-B"), "ORDER-1");

    EXPECT_FALSE(first == second);
}

// The point of the whole change, stated as the thing a member can now do. The same member
// reconnecting -- on a new connection, and after a failover on a different instance -- must
// produce the same key, or its resting orders are unmanageable: visible on the book,
// cancellable by nobody. Nothing about the connection or the instance appears in the key,
// which is what makes this hold rather than being a happy accident of numbering.
TEST(OrderKeyTest, AReconnectedSessionProducesTheSameKeyAndCanCancelWhatItLeftResting) {
    OrderBook book;
    // Placed through instance a, on whatever connection it had at the time.
    const OrderKey when_placed = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    book.emplace(when_placed, "resting");

    // The gateway dies; the member logs on to instance b and cancels. Only its identity is
    // the same -- the process serving it and the connection number are both different.
    const OrderKey after_reconnect = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");

    ASSERT_EQ(book.count(after_reconnect), 1U) << "a reconnected session could not find the order it left resting";
    book.erase(after_reconnect);
    EXPECT_EQ(book.size(), 0U);
}

// The cross-protocol case, which the identity keeps apart on purpose: one member holding a
// FIX session and a binary session holds two sessions, and an order in one is not an order
// in the other. Without the protocol in the identity these would be one book entry, and a
// cancel on either would retire the other's order.
TEST(OrderKeyTest, TheSameCompIdOnTwoProtocolsIsTwoSessions) {
    const OrderKey via_fix = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    const OrderKey via_binary = OrderKey::make(binary_session("MEMBER-A"), "ORDER-1");

    EXPECT_FALSE(via_fix == via_binary);
    EXPECT_NE(OrderKeyHash{}(via_fix), OrderKeyHash{}(via_binary));
}

TEST(OrderKeyTest, OrdersFromTwoProtocolsCoexistAndCancelIndependently) {
    OrderBook book;
    const OrderKey via_fix = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");
    const OrderKey via_binary = OrderKey::make(binary_session("MEMBER-A"), "ORDER-1");

    book.emplace(via_fix, "from the FIX gateway");
    book.emplace(via_binary, "from the binary gateway");
    ASSERT_EQ(book.size(), 2U) << "the two orders collapsed into one book entry";

    book.erase(via_fix);
    EXPECT_EQ(book.count(via_fix), 0U);
    ASSERT_EQ(book.count(via_binary), 1U) << "cancelling one protocol's order retired the other's";
    EXPECT_EQ(book.at(via_binary), "from the binary gateway");
}

// An absent gateway id means the FIX order gateway, so a record whose envelope carried no
// protocol keys the same as one that named FIX explicitly.
TEST(OrderKeyTest, AnAbsentProtocolMeansTheFixOrderGateway) {
    const OrderKey defaulted = OrderKey::make(SessionIdentity::make("MEMBER-A", gateway_ids::default_when_absent), "ORDER-1");
    const OrderKey explicit_fix = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");

    EXPECT_TRUE(defaulted == explicit_fix);
}

// The key truncates at the shared ClOrdID limit, which is only safe because the gateways
// reject longer ones at ingress. If that check is ever dropped, this is where it bites.
TEST(OrderKeyTest, ClOrdIdIsTruncatedAtTheSharedMaximum) {
    const std::string over_long(fix_order_limits::max_cl_ord_id_length + 10, 'X');
    const OrderKey key = OrderKey::make(fix_session("MEMBER-A"), over_long);

    EXPECT_EQ(key.cl_ord_id_len, fix_order_limits::max_cl_ord_id_length);
}

// The comp id half of the identity truncates at the database column's width. Nothing that
// can authenticate is longer, so this is a guard on that assumption rather than a live
// path: if the column is ever widened without widening max_comp_id_length, two comp ids
// sharing a 64-character prefix become one session, and this is where that shows up.
TEST(OrderKeyTest, CompIdIsTruncatedAtTheSharedMaximum) {
    const std::string over_long(fix_order_limits::max_comp_id_length + 10, 'M');
    const SessionIdentity identity = SessionIdentity::make(over_long, gateway_ids::fix_order_gateway);

    EXPECT_EQ(identity.comp_id_len, fix_order_limits::max_comp_id_length);
}

// A session with no comp id is a record with no originating client at all -- a replication
// record, or a report the matching engine emitted with no order behind it. It must not
// silently key the same as a real member.
TEST(OrderKeyTest, AnEmptyIdentityIsNotAnyMembersSession) {
    const OrderKey anonymous = OrderKey::make(SessionIdentity{}, "ORDER-1");
    const OrderKey member = OrderKey::make(fix_session("MEMBER-A"), "ORDER-1");

    EXPECT_TRUE(SessionIdentity{}.empty());
    EXPECT_FALSE(anonymous == member);
}

} // namespaces
