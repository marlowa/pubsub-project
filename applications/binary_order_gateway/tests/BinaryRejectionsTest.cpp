// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include <fix_orders.hpp>

#include "BinaryRejections.hpp"
#include "GatewayRefusal.hpp"

namespace binary_order_gateway::tests {

namespace {

constexpr int64_t refused_at_ns = 1'700'000'000'000'000'000;

} // namespaces

TEST(BinaryRejectionsTest, ANewOrderWhoseClOrdIdIsInUseIsRejectedAsADuplicateOrder) {
    pubsub_itc_fw_app::NewOrderSingleView order{};
    order.cl_ord_id = "ORDER-1";
    order.side = pubsub_itc_fw_app::Side::Sell;
    order.symbol = "BHP";
    order.order_qty = "250";

    const pubsub_itc_fw_app::ExecutionReport report = make_reject_report("GW-ORD-1", "GW-EXEC-1", order, fix_common::identifier_already_used_text,
                                                                         pubsub_itc_fw_app::OrdRejReason::DuplicateOrder, refused_at_ns);
    EXPECT_EQ(report.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(report.ord_status, pubsub_itc_fw_app::OrdStatus::Rejected);
    ASSERT_TRUE(report.has_ord_rej_reason);
    EXPECT_EQ(report.ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::DuplicateOrder);
    ASSERT_TRUE(report.has_text);
    EXPECT_EQ(report.text, "ClOrdID already used today");
    ASSERT_TRUE(report.has_cl_ord_id);
    EXPECT_EQ(report.cl_ord_id, "ORDER-1");
    EXPECT_EQ(report.symbol, "BHP");
    EXPECT_EQ(report.side, pubsub_itc_fw_app::Side::Sell);
    EXPECT_EQ(report.order_qty, "250");
    EXPECT_EQ(report.leaves_qty, "0");
    EXPECT_EQ(report.cum_qty, "0");
    EXPECT_EQ(report.transact_time, refused_at_ns);
}

TEST(BinaryRejectionsTest, ACancelWhoseClOrdIdIsInUseIsRefusedWithoutSayingAnythingOfTheOrder) {
    pubsub_itc_fw_app::OrderCancelRequestView request{};
    request.cl_ord_id = "CANCEL-1";
    request.orig_cl_ord_id = "ORDER-1";

    const pubsub_itc_fw_app::RequestRefused refused =
        make_cancel_refused(request, fix_common::RefusalReason::Other, fix_common::identifier_already_used_text, std::string_view());
    EXPECT_EQ(refused.cl_ord_id, "CANCEL-1");
    EXPECT_EQ(refused.refused_pdu_id, pubsub_itc_fw_app::OrderCancelRequest::message_pdu_id);
    EXPECT_EQ(static_cast<int32_t>(refused.reason), 99) << "99 is Other, as in the FIX gateway's SessionRejectReason";
    EXPECT_EQ(refused.text, "ClOrdID already used today");
    EXPECT_FALSE(refused.has_field) << "no one field is at fault";
}

TEST(BinaryRejectionsTest, ACancelWithAFieldAtFaultNamesTheField) {
    pubsub_itc_fw_app::OrderCancelRequestView request{};
    request.cl_ord_id = "CANCEL-1";

    const pubsub_itc_fw_app::RequestRefused refused =
        make_cancel_refused(request, fix_common::RefusalReason::ValueIncorrect, "side is missing or holds a value the protocol does not define", "side");
    EXPECT_EQ(static_cast<int32_t>(refused.reason), 5);
    ASSERT_TRUE(refused.has_field);
    EXPECT_EQ(refused.field, "side");
}

} // namespaces
