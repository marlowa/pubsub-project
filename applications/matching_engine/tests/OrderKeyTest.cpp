// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <OrderKey.hpp>

#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"

// The order book's key decides which orders are the same order. These tests pin the three
// things it must distinguish, the middle one having been got wrong once: the gateway id
// was missing, so two gateways shared a session-id space and a client on one could collide
// with -- or cancel -- an order belonging to a client on the other.

using matching_engine::OrderKey;
using matching_engine::OrderKeyHash;

namespace {

using OrderBook = std::unordered_map<OrderKey, std::string, OrderKeyHash>;

TEST(OrderKeyTest, SameGatewaySessionAndClOrdIdIsTheSameOrder) {
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");
    const OrderKey second = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");

    EXPECT_TRUE(first == second);
    EXPECT_EQ(OrderKeyHash{}(first), OrderKeyHash{}(second));
}

TEST(OrderKeyTest, DifferentClOrdIdOnOneSessionIsADifferentOrder) {
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");
    const OrderKey second = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-2");

    EXPECT_FALSE(first == second);
}

TEST(OrderKeyTest, DifferentSessionOnOneGatewayIsADifferentOrder) {
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");
    const OrderKey second = OrderKey::make(6, gateway_ids::fix_order_gateway, "ORDER-1");

    EXPECT_FALSE(first == second);
}

// The regression. Each gateway numbers its own client connections, so connection 5 exists
// on both; without the gateway id these two unrelated orders were one book entry.
TEST(OrderKeyTest, SameSessionNumberOnDifferentGatewaysIsADifferentOrder) {
    const OrderKey via_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");
    const OrderKey via_binary = OrderKey::make(5, gateway_ids::binary_order_gateway, "ORDER-1");

    EXPECT_FALSE(via_fix == via_binary);
}

// The consequence that matters, stated in the terms the bug would have shown up in: two
// clients placing the same ClOrdID through different gateways must both rest on the book,
// and cancelling one must not retire the other.
TEST(OrderKeyTest, OrdersFromTwoGatewaysCoexistAndCancelIndependently) {
    OrderBook book;
    const OrderKey via_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");
    const OrderKey via_binary = OrderKey::make(5, gateway_ids::binary_order_gateway, "ORDER-1");

    book.emplace(via_fix, "from the FIX gateway");
    book.emplace(via_binary, "from the binary gateway");
    ASSERT_EQ(book.size(), 2U) << "the two orders collapsed into one book entry";

    book.erase(via_fix);
    EXPECT_EQ(book.count(via_fix), 0U);
    ASSERT_EQ(book.count(via_binary), 1U) << "cancelling one gateway's order retired the other's";
    EXPECT_EQ(book.at(via_binary), "from the binary gateway");
}

// An absent gateway id means the FIX order gateway, so orders replayed from a WAL written
// before gateway ids existed keep the identity they had when they were written.
TEST(OrderKeyTest, AnAbsentGatewayIdMeansTheOrderGateway) {
    const OrderKey defaulted = OrderKey::make(5, gateway_ids::default_when_absent, "ORDER-1");
    const OrderKey explicit_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, "ORDER-1");

    EXPECT_TRUE(defaulted == explicit_fix);
}

// The key truncates at the shared ClOrdID limit, which is only safe because the gateways
// reject longer ones at ingress. If that check is ever dropped, this is where it bites.
TEST(OrderKeyTest, ClOrdIdIsTruncatedAtTheSharedMaximum) {
    const std::string over_long(fix_order_limits::max_cl_ord_id_length + 10, 'X');
    const OrderKey key = OrderKey::make(5, gateway_ids::fix_order_gateway, over_long);

    EXPECT_EQ(key.cl_ord_id_len, fix_order_limits::max_cl_ord_id_length);
}

} // namespaces
