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

TEST(BinaryRejectionsTest, ACancelWhoseClOrdIdIsInUseIsRefusedAsADuplicateClOrdIdAndTheOrderStaysOpen) {
    pubsub_itc_fw_app::OrderCancelRequestView request{};
    request.cl_ord_id = "CANCEL-1";
    request.orig_cl_ord_id = "ORDER-1";

    const pubsub_itc_fw_app::OrderCancelReject reject =
        make_cancel_reject("NONE", request, fix_common::identifier_already_used_text, pubsub_itc_fw_app::CxlRejReason::DuplicateClOrdId, refused_at_ns);
    ASSERT_TRUE(reject.has_cxl_rej_reason);
    EXPECT_EQ(reject.cxl_rej_reason, pubsub_itc_fw_app::CxlRejReason::DuplicateClOrdId);
    EXPECT_EQ(reject.ord_status, pubsub_itc_fw_app::OrdStatus::New) << "the refusal has not touched the order, so it is still open";
    EXPECT_EQ(reject.cxl_rej_response_to, pubsub_itc_fw_app::CxlRejResponseTo::OrderCancelRequest);
    EXPECT_EQ(reject.cl_ord_id, "CANCEL-1");
    ASSERT_TRUE(reject.has_orig_cl_ord_id);
    EXPECT_EQ(reject.orig_cl_ord_id, "ORDER-1");
    EXPECT_EQ(reject.order_id, "NONE");
    ASSERT_TRUE(reject.has_text);
    EXPECT_EQ(reject.text, "ClOrdID already used today");
}

} // namespaces
