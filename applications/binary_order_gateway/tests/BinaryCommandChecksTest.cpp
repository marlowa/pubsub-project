// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_orders.hpp>

#include "BinaryCommandChecks.hpp"
#include "FixOrderLimits.hpp"

namespace binary_order_gateway::tests {

namespace {

using pubsub_itc_fw_app::NewOrderSingleView;
using pubsub_itc_fw_app::OrderCancelRequestView;

const CommandFieldLimits limits{32, 24};

/// A new order every check accepts. Each test changes one thing about it.
NewOrderSingleView valid_order() {
    NewOrderSingleView order{};
    order.cl_ord_id = "ORDER-1";
    order.side = pubsub_itc_fw_app::Side::Buy;
    order.symbol = "BHP";
    order.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    order.order_qty = "100";
    order.has_price = true;
    order.price = "10.50";
    return order;
}

/// A cancel every check accepts, with no OrderQty, which a cancel need not carry (R-0142).
OrderCancelRequestView valid_cancel() {
    OrderCancelRequestView request{};
    request.cl_ord_id = "CANCEL-1";
    request.orig_cl_ord_id = "ORDER-1";
    request.side = pubsub_itc_fw_app::Side::Buy;
    request.symbol = "BHP";
    return request;
}

class BinaryCommandChecksTest : public ::testing::Test {
  protected:
    std::string_view check(const NewOrderSingleView& order) {
        return check_new_order(order, limits, buffer_, sizeof(buffer_));
    }

    std::string_view check(const OrderCancelRequestView& request) {
        return check_cancel(request, limits, buffer_, sizeof(buffer_));
    }

    char buffer_[command_refusal_text_capacity]{};
};

} // namespaces

TEST_F(BinaryCommandChecksTest, AValidOrderPasses) {
    EXPECT_EQ(check(valid_order()), "");
}

TEST_F(BinaryCommandChecksTest, ASideTheProtocolDoesNotDefineIsRefused) {
    NewOrderSingleView order = valid_order();
    order.side = static_cast<pubsub_itc_fw_app::Side>('Z');
    EXPECT_EQ(check(order), "side is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, AnOrdTypeTheProtocolDoesNotDefineIsRefused) {
    NewOrderSingleView order = valid_order();
    order.ord_type = static_cast<pubsub_itc_fw_app::OrdType>('#');
    EXPECT_EQ(check(order), "ord_type is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, AnOptionalEnumeratedFieldIsCheckedOnlyWhenPresent) {
    NewOrderSingleView order = valid_order();
    order.time_in_force = static_cast<pubsub_itc_fw_app::TimeInForce>('!');
    EXPECT_EQ(check(order), "") << "an absent TimeInForce was checked";
    order.has_time_in_force = true;
    EXPECT_EQ(check(order), "time_in_force is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, AnEmptyRequiredStringIsRefused) {
    NewOrderSingleView order = valid_order();
    order.symbol = "";
    EXPECT_EQ(check(order), "symbol is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, AnInvalidValueInsideARepeatingGroupIsRefused) {
    pubsub_itc_fw_app::PartyIDsView party{};
    party.has_party_id_source = true;
    party.party_id_source = static_cast<pubsub_itc_fw_app::PartyIDSource>('@');
    NewOrderSingleView order = valid_order();
    order.no_party_i_ds = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDsView>{&party, 1};
    EXPECT_EQ(check(order), "party_id_source is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, AClOrdIdLongerThanTheBookKeyIsRefused) {
    const std::string longest(fix_order_limits::max_cl_ord_id_length, 'A');
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'A');
    NewOrderSingleView order = valid_order();
    order.cl_ord_id = longest;
    EXPECT_EQ(check(order), "");
    order.cl_ord_id = too_long;
    EXPECT_EQ(check(order), "cl_ord_id exceeds maximum length of 64");
}

TEST_F(BinaryCommandChecksTest, SymbolAndQuantityAreHeldToTheConfiguredLengths) {
    const std::string long_symbol(33, 'S');
    NewOrderSingleView order = valid_order();
    order.symbol = long_symbol;
    EXPECT_EQ(check(order), "symbol exceeds maximum length of 32");

    const std::string long_quantity(25, '1');
    order = valid_order();
    order.order_qty = long_quantity;
    EXPECT_EQ(check(order), "order_qty exceeds maximum length of 24");
}

TEST_F(BinaryCommandChecksTest, QuantitiesAndPricesMustBeDecimalNumbers) {
    NewOrderSingleView order = valid_order();
    order.order_qty = "1e3";
    EXPECT_EQ(check(order), "order_qty is not a decimal number");

    order = valid_order();
    order.price = "ten";
    EXPECT_EQ(check(order), "price is not a decimal number");

    order = valid_order();
    order.has_stop_px = true;
    order.stop_px = "9.5.1";
    EXPECT_EQ(check(order), "stop_px is not a decimal number");

    order = valid_order();
    order.has_min_qty = true;
    order.min_qty = "-";
    EXPECT_EQ(check(order), "min_qty is not a decimal number");

    order = valid_order();
    order.has_max_floor = true;
    order.max_floor = "50 ";
    EXPECT_EQ(check(order), "max_floor is not a decimal number");
}

TEST_F(BinaryCommandChecksTest, AnUnderlyingQuantityMustBeADecimalNumber) {
    pubsub_itc_fw_app::UnderlyingsView underlying{};
    underlying.has_underlying_qty = true;
    underlying.underlying_qty = "lots";
    NewOrderSingleView order = valid_order();
    order.no_underlyings = pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::UnderlyingsView>{&underlying, 1};
    EXPECT_EQ(check(order), "underlying_qty is not a decimal number");
}

TEST_F(BinaryCommandChecksTest, AValidCancelWithoutAQuantityPasses) {
    EXPECT_EQ(check(valid_cancel()), "");
}

TEST_F(BinaryCommandChecksTest, ACancelsIdentifiersAreHeldToTheBookKeyLength) {
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'A');
    OrderCancelRequestView request = valid_cancel();
    request.orig_cl_ord_id = too_long;
    EXPECT_EQ(check(request), "orig_cl_ord_id exceeds maximum length of 64");
    request = valid_cancel();
    request.cl_ord_id = too_long;
    EXPECT_EQ(check(request), "cl_ord_id exceeds maximum length of 64");
}

TEST_F(BinaryCommandChecksTest, ACancelWithoutAnOrigClOrdIdIsRefused) {
    OrderCancelRequestView request = valid_cancel();
    request.orig_cl_ord_id = "";
    EXPECT_EQ(check(request), "orig_cl_ord_id is missing or holds a value the protocol does not define");
}

TEST_F(BinaryCommandChecksTest, ACancelsQuantityWhenGivenMustBeADecimalNumber) {
    OrderCancelRequestView request = valid_cancel();
    request.has_order_qty = true;
    request.order_qty = "100";
    EXPECT_EQ(check(request), "");
    request.order_qty = "abc";
    EXPECT_EQ(check(request), "order_qty is not a decimal number");
}

TEST_F(BinaryCommandChecksTest, AReasonLongerThanTheBufferIsCutShortNotOverrun) {
    char small_buffer[8]{};
    NewOrderSingleView order = valid_order();
    order.side = static_cast<pubsub_itc_fw_app::Side>('Z');
    const std::string_view reason = check_new_order(order, limits, small_buffer, sizeof(small_buffer));
    EXPECT_EQ(reason, "side is ");
    EXPECT_EQ(reason.data(), small_buffer);
}

} // namespaces
