// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixErEncoder.hpp>

#include <cstddef>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>
#include <fix_equity_orders.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

// Guards the ExecutionReport encoder after its move onto fix_codec::FixMessageWriter.
// The returned view does not start at buffer[0] (the header is framed backward into
// a reserved prefix), so parsing the view proves both the framing and that the
// caller must use view.data()/view.size(), not the buffer base.

using order_gateway::encode_execution_report;
namespace tag = fix_codec::tag;

namespace {

TEST(FixErEncoderTest, EncodesAValidExecutionReport) {
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "ME-ORD-1";
    view.exec_id = "ME-EXEC-1";
    view.exec_type = static_cast<pubsub_itc_fw_app::ExecType>('0');   // New
    view.ord_status = static_cast<pubsub_itc_fw_app::OrdStatus>('0'); // New
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1'); // Buy
    view.leaves_qty = "100";
    view.cum_qty = "0";
    view.has_cl_ord_id = true;
    view.cl_ord_id = "ORDER-1";
    view.has_order_qty = true;
    view.order_qty = "100";

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[order_gateway::execution_report_buffer_size];
    const std::string_view wire = encode_execution_report(view, "GATEWAY", "CLIENT", 5, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire; // BodyLength and Checksum are correct
    EXPECT_EQ(reader.msg_type(), "8");
    EXPECT_EQ(reader.find(tag::ClOrdID).as_string_view(), "ORDER-1");
    EXPECT_EQ(reader.find(tag::OrderID).as_string_view(), "ME-ORD-1");
    EXPECT_EQ(reader.find(tag::ExecID).as_string_view(), "ME-EXEC-1");
    EXPECT_EQ(reader.find(tag::ExecType).as_char(), '0');
    EXPECT_EQ(reader.find(tag::OrdStatus).as_char(), '0');
    EXPECT_EQ(reader.find(tag::Symbol).as_string_view(), "AAPL");
    EXPECT_EQ(reader.find(tag::Side).as_char(), '1');
    EXPECT_EQ(reader.find(tag::OrderQty).as_string_view(), "100");
    EXPECT_EQ(reader.find(tag::LeavesQty).as_string_view(), "100");
}

} // namespaces
