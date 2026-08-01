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
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");
    const OrderKey second = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");

    EXPECT_TRUE(first == second);
    EXPECT_EQ(OrderKeyHash{}(first), OrderKeyHash{}(second));
}

TEST(OrderKeyTest, DifferentClOrdIdOnOneSessionIsADifferentOrder) {
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");
    const OrderKey second = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-2");

    EXPECT_FALSE(first == second);
}

TEST(OrderKeyTest, DifferentSessionOnOneGatewayIsADifferentOrder) {
    const OrderKey first = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");
    const OrderKey second = OrderKey::make(6, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");

    EXPECT_FALSE(first == second);
}

// The regression. Each gateway numbers its own client connections, so connection 5 exists
// on both; without the gateway id these two unrelated orders were one book entry.
TEST(OrderKeyTest, SameSessionNumberOnDifferentGatewaysIsADifferentOrder) {
    const OrderKey via_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");
    const OrderKey via_binary = OrderKey::make(5, gateway_ids::binary_order_gateway, gateway_ids::first_instance, "ORDER-1");

    EXPECT_FALSE(via_fix == via_binary);
}

// The consequence that matters, stated in the terms the bug would have shown up in: two
// clients placing the same ClOrdID through different gateways must both rest on the book,
// and cancelling one must not retire the other.
TEST(OrderKeyTest, OrdersFromTwoGatewaysCoexistAndCancelIndependently) {
    OrderBook book;
    const OrderKey via_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");
    const OrderKey via_binary = OrderKey::make(5, gateway_ids::binary_order_gateway, gateway_ids::first_instance, "ORDER-1");

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
    const OrderKey defaulted = OrderKey::make(5, gateway_ids::default_when_absent, gateway_ids::first_instance, "ORDER-1");
    const OrderKey explicit_fix = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, "ORDER-1");

    EXPECT_TRUE(defaulted == explicit_fix);
}

// The key truncates at the shared ClOrdID limit, which is only safe because the gateways
// reject longer ones at ingress. If that check is ever dropped, this is where it bites.
TEST(OrderKeyTest, ClOrdIdIsTruncatedAtTheSharedMaximum) {
    const std::string over_long(fix_order_limits::max_cl_ord_id_length + 10, 'X');
    const OrderKey key = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::first_instance, over_long);

    EXPECT_EQ(key.cl_ord_id_len, fix_order_limits::max_cl_ord_id_length);
}

// The second regression, and the reason `make` takes the instance rather than defaulting
// it. The argument is the same one that motivated the gateway id, applied one level down:
// each gateway *instance* numbers its own client connections, so connection 5 exists on
// instance a and on instance b and they are unrelated sessions.
TEST(OrderKeyTest, SameSessionNumberOnDifferentInstancesOfOneGatewayIsADifferentOrder) {
    const OrderKey via_a = OrderKey::make(5, gateway_ids::fix_order_gateway, 1, "ORDER-1");
    const OrderKey via_b = OrderKey::make(5, gateway_ids::fix_order_gateway, 2, "ORDER-1");

    EXPECT_FALSE(via_a == via_b);
}

// Stated as the damage it prevents, as the cross-gateway case above is: two members on
// different instances of the SAME protocol may pick the same ClOrdID, and cancelling one
// must not retire the other. This is the case that became reachable the moment a second
// instance of a protocol started taking orders.
TEST(OrderKeyTest, OrdersFromTwoInstancesOfOneGatewayCoexistAndCancelIndependently) {
    OrderBook book;
    const OrderKey via_a = OrderKey::make(5, gateway_ids::fix_order_gateway, 1, "ORDER-1");
    const OrderKey via_b = OrderKey::make(5, gateway_ids::fix_order_gateway, 2, "ORDER-1");

    book.emplace(via_a, "from FIX instance a");
    book.emplace(via_b, "from FIX instance b");
    ASSERT_EQ(book.size(), 2U) << "the two orders collapsed into one book entry";

    book.erase(via_a);
    EXPECT_EQ(book.count(via_a), 0U);
    ASSERT_EQ(book.count(via_b), 1U) << "cancelling instance a's order retired instance b's";
    EXPECT_EQ(book.at(via_b), "from FIX instance b");
}

// A protocol and an instance are separate axes, so the same number in each must not be
// interchangeable: (binary, instance 1) is not (FIX, instance 2) however the two are
// combined. Guards against any future hash or equality that folds the pair into one value.
TEST(OrderKeyTest, ProtocolAndInstanceAreNotInterchangeable) {
    const OrderKey fix_instance_two = OrderKey::make(5, gateway_ids::fix_order_gateway, 2, "ORDER-1");
    const OrderKey binary_instance_one = OrderKey::make(5, gateway_ids::binary_order_gateway, 1, "ORDER-1");

    EXPECT_FALSE(fix_instance_two == binary_instance_one);
    EXPECT_NE(OrderKeyHash{}(fix_instance_two), OrderKeyHash{}(binary_instance_one));
}

// Absent means instance 1, matching gateway_ids::default_instance_when_absent, so a record
// written before the instance axis existed keeps the identity it had when it was written.
TEST(OrderKeyTest, AnAbsentInstanceMeansTheFirstInstance) {
    const OrderKey defaulted = OrderKey::make(5, gateway_ids::fix_order_gateway, gateway_ids::default_instance_when_absent, "ORDER-1");
    const OrderKey explicit_first = OrderKey::make(5, gateway_ids::fix_order_gateway, 1, "ORDER-1");

    EXPECT_TRUE(defaulted == explicit_first);
}

} // namespaces
