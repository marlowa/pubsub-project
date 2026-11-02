// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixRejections.hpp>

#include <gtest/gtest.h>

#include <fix_orders.hpp>

#include <GatewayRefusal.hpp>

using fix_order_gateway::FixMessage;
using fix_order_gateway::make_order_cancel_reject;
using fix_order_gateway::make_reject_execution_report;
namespace Tag = fix_codec::tag;

TEST(FixRejectionsTest, ANewOrderWhoseClOrdIdIsInUseIsRejectedAsADuplicateOrder) {
    const FixMessage er = make_reject_execution_report("GW-ORD-1", "GW-EXEC-1", "ORDER-1", "BHP", "2", "250", fix_common::identifier_already_used_text,
                                                       pubsub_itc_fw_app::OrdRejReason::DuplicateOrder);
    EXPECT_EQ(er.get(Tag::MsgType), "8");
    EXPECT_EQ(er.get(Tag::ExecType), "8") << "150 must say Rejected";
    EXPECT_EQ(er.get(Tag::OrdStatus), "8") << "39 must say Rejected";
    EXPECT_EQ(er.get(Tag::OrdRejReason), "6") << "103 = 6 is duplicate order";
    EXPECT_EQ(er.get(Tag::Text), "ClOrdID already used today");
    EXPECT_EQ(er.get(Tag::ClOrdID), "ORDER-1");
    EXPECT_EQ(er.get(Tag::Symbol), "BHP");
    EXPECT_EQ(er.get(Tag::Side), "2");
    EXPECT_EQ(er.get(Tag::OrderQty), "250");
    EXPECT_EQ(er.get(Tag::LeavesQty), "0");
    EXPECT_EQ(er.get(Tag::CumQty), "0");
    EXPECT_EQ(er.get(Tag::OrderID), "GW-ORD-1");
}

TEST(FixRejectionsTest, ARejectionWithoutSideOrQuantityLeavesThoseTagsOut) {
    const FixMessage er =
        make_reject_execution_report("GW-ORD-1", "GW-EXEC-1", "ORDER-1", "BHP", "", "", "the venue is busy", pubsub_itc_fw_app::OrdRejReason::Other);
    EXPECT_EQ(er.get(Tag::OrdRejReason), "99");
    EXPECT_TRUE(er.get(Tag::Side).empty());
    EXPECT_TRUE(er.get(Tag::OrderQty).empty());
    EXPECT_TRUE(er.get(Tag::LeavesQty).empty());
}

TEST(FixRejectionsTest, ACancelWhoseClOrdIdIsInUseIsRefusedAsADuplicateClOrdIdAndTheOrderStaysOpen) {
    const FixMessage reject =
        make_order_cancel_reject("NONE", "CANCEL-1", "ORDER-1", fix_common::identifier_already_used_text, pubsub_itc_fw_app::CxlRejReason::DuplicateClOrdId);
    EXPECT_EQ(reject.get(Tag::MsgType), "9");
    EXPECT_EQ(reject.get(Tag::CxlRejReason), "6") << "102 = 6 is duplicate ClOrdID received";
    EXPECT_EQ(reject.get(Tag::Text), "ClOrdID already used today");
    EXPECT_EQ(reject.get(Tag::OrdStatus), "0") << "the refusal has not touched the order, so it is still open";
    EXPECT_EQ(reject.get(Tag::CxlRejResponseTo), "1");
    EXPECT_EQ(reject.get(Tag::ClOrdID), "CANCEL-1");
    EXPECT_EQ(reject.get(Tag::OrigClOrdID), "ORDER-1");
    EXPECT_EQ(reject.get(Tag::OrderID), "NONE");
}

TEST(FixRejectionsTest, ACancelRefusedForAReasonOfTheGatewaysOwnSaysOther) {
    const FixMessage reject = make_order_cancel_reject("NONE", "CANCEL-1", "", "the venue is busy", pubsub_itc_fw_app::CxlRejReason::Other);
    EXPECT_EQ(reject.get(Tag::CxlRejReason), "99");
    EXPECT_TRUE(reject.get(Tag::OrigClOrdID).empty());
}
