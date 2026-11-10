// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixRejections.hpp>

#include <gtest/gtest.h>

#include <fix_orders.hpp>

#include <GatewayRefusal.hpp>

using fix_order_gateway::FixMessage;
using fix_order_gateway::make_reject_execution_report;
using fix_order_gateway::make_request_reject;
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

TEST(FixRejectionsTest, ACancelTheGatewayRefusesIsAnsweredWithASessionLevelRejectNamingTheMessage) {
    const FixMessage reject = make_request_reject("42", "F", 0, 100, "Throttled: at most 3 cancels per second for this session");
    EXPECT_EQ(reject.get(Tag::MsgType), "3");
    EXPECT_EQ(reject.get(Tag::RefSeqNum), "42");
    EXPECT_EQ(reject.get(Tag::RefMsgType), "F");
    EXPECT_EQ(reject.get(Tag::SessionRejectReason), "100");
    EXPECT_EQ(reject.get(Tag::Text), "Throttled: at most 3 cancels per second for this session");
    EXPECT_TRUE(reject.get(Tag::RefTagID).empty()) << "no one field is at fault, so tag 371 is left out";
    EXPECT_TRUE(reject.get(Tag::OrderID).empty()) << "the Reject says nothing of the order";
    EXPECT_TRUE(reject.get(Tag::OrdStatus).empty()) << "the Reject says nothing of the order";
}

TEST(FixRejectionsTest, ARejectForAMissingFieldNamesTheField) {
    const FixMessage reject = make_request_reject("7", "F", 41, 1, "missing required field(s): OrigClOrdID");
    EXPECT_EQ(reject.get(Tag::RefTagID), "41");
    EXPECT_EQ(reject.get(Tag::SessionRejectReason), "1");
}
