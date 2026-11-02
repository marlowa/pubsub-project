// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "RefusedCommand.hpp"

namespace fix_common::tests {

namespace {

// Encodes a message into bytes of its own.
template <typename Message> std::vector<uint8_t> encoded(const Message& message) {
    size_t written = 0;
    size_t needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(message, nullptr, 0, written, needed));
    std::vector<uint8_t> bytes(needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(message, bytes.data(), bytes.size(), written, needed));
    bytes.resize(written);
    return bytes;
}

// A CommandRefused carrying @p command, a command of PDU id @p command_pdu_id, encoded and decoded again as a gateway
// receives it.
class RefusedCommandTest : public ::testing::Test {
  protected:
    pubsub_itc_fw_app::CommandRefusedView refusal_carrying(int16_t command_pdu_id, const std::vector<uint8_t>& command) {
        pubsub_itc_fw_app::CommandRefused refused{};
        refused.gateway_session_conn_id = 7;
        refused.sender_comp_id = "MEMBER-A";
        refused.cl_ord_id = "ORDER-1";
        refused.command_pdu_id = command_pdu_id;
        refused.command.data = command.data();
        refused.command.size = command.size();
        refused.reason = pubsub_itc_fw_app::CommandRefusedReason::IdentifierAlreadyUsed;
        refused.first_seq_no = 42;
        refusal_bytes_ = encoded(refused);

        pubsub_itc_fw_app::CommandRefusedView view{};
        size_t consumed = 0;
        size_t needed = 0;
        EXPECT_TRUE(pubsub_itc_fw_app::decode(view, refusal_bytes_.data(), refusal_bytes_.size(), consumed, arena_, needed));
        return view;
    }

    std::array<uint8_t, 4096> arena_buffer_{};
    pubsub_itc_fw::BumpAllocator arena_{arena_buffer_.data(), arena_buffer_.size()};
    std::vector<uint8_t> refusal_bytes_;
};

} // namespaces

TEST_F(RefusedCommandTest, ANewOrderIsDecodedWithTheFieldsARejectionEchoes) {
    pubsub_itc_fw_app::NewOrderSingle order{};
    order.cl_ord_id = "ORDER-1";
    order.side = pubsub_itc_fw_app::Side::Sell;
    order.symbol = "BHP";
    order.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    order.order_qty = "250";
    const std::vector<uint8_t> command = encoded(order);
    const pubsub_itc_fw_app::CommandRefusedView view = refusal_carrying(pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, command);
    EXPECT_EQ(view.gateway_session_conn_id, 7);
    EXPECT_EQ(view.sender_comp_id, "MEMBER-A");
    EXPECT_EQ(view.first_seq_no, 42);

    const auto [refused, why_not] = decode_refused_command(view, arena_);
    ASSERT_TRUE(refused.has_value()) << why_not;
    EXPECT_EQ(refused->kind, RefusedCommand::Kind::NewOrder);
    EXPECT_EQ(refused->order.cl_ord_id, "ORDER-1");
    EXPECT_EQ(refused->order.symbol, "BHP");
    EXPECT_EQ(refused->order.side, pubsub_itc_fw_app::Side::Sell);
    EXPECT_EQ(refused->order.order_qty, "250");
}

TEST_F(RefusedCommandTest, ARequestToCancelIsDecodedWithTheOrderItNames) {
    pubsub_itc_fw_app::OrderCancelRequest request{};
    request.orig_cl_ord_id = "ORDER-1";
    request.cl_ord_id = "CANCEL-1";
    request.side = pubsub_itc_fw_app::Side::Buy;
    request.symbol = "BHP";
    const std::vector<uint8_t> command = encoded(request);
    const pubsub_itc_fw_app::CommandRefusedView view = refusal_carrying(pubsub_itc_fw_app::OrderCancelRequest::message_pdu_id, command);

    const auto [refused, why_not] = decode_refused_command(view, arena_);
    ASSERT_TRUE(refused.has_value()) << why_not;
    EXPECT_EQ(refused->kind, RefusedCommand::Kind::Cancel);
    EXPECT_EQ(refused->cancel.cl_ord_id, "CANCEL-1");
    EXPECT_EQ(refused->cancel.orig_cl_ord_id, "ORDER-1");
}

TEST_F(RefusedCommandTest, ACommandThatCannotBeDecodedIsReportedAndNotAnswered) {
    const std::vector<uint8_t> command{0x01, 0x02};
    const pubsub_itc_fw_app::CommandRefusedView view = refusal_carrying(pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, command);

    const auto [refused, why_not] = decode_refused_command(view, arena_);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(why_not, "the NewOrderSingle it carries cannot be decoded");
}

TEST_F(RefusedCommandTest, ACommandThatIsNeitherANewOrderNorACancelIsReportedAndNotAnswered) {
    const std::vector<uint8_t> command{0x01, 0x02};
    const pubsub_itc_fw_app::CommandRefusedView view = refusal_carrying(pubsub_itc_fw_app::ExecutionReport::message_pdu_id, command);

    const auto [refused, why_not] = decode_refused_command(view, arena_);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(why_not, "it carries a command that is neither a NewOrderSingle nor an OrderCancelRequest");
}

} // namespaces
