// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>

#include <binary_session.hpp>
#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "GatewayIds.hpp"

// Pins the two wire contracts the binary gateway rests on: the session protocol its
// clients speak, and the envelope field that decides which gateway an ExecutionReport
// comes back to. Both are protocol promises rather than implementation details, so a
// change that breaks either should fail here rather than in a live venue.

namespace {

constexpr size_t arena_size = 16 * 1024;

/** @brief Encodes a message into a right-sized buffer, as the send path does. */
template <typename MessageType> std::vector<uint8_t> encode_message(const MessageType& message) {
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    // The measuring pass reports a zero-size buffer as too small but still sets
    // bytes_needed, which is the point of the call; its result is not an error.
    static_cast<void>(encode(message, nullptr, 0, bytes_written, bytes_needed));

    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(encode(message, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

class BinaryProtocolTest : public ::testing::Test {
  protected:
    std::vector<uint8_t> arena_buffer_ = std::vector<uint8_t>(arena_size);
};

TEST_F(BinaryProtocolTest, LogonCarriesTheCompIdIntactAcrossTheWire) {
    pubsub_itc_fw_app::Logon logon{};
    logon.comp_id = "BINCLIENT";
    const std::vector<uint8_t> wire = encode_message(logon);

    pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::LogonView view{};
    ASSERT_TRUE(pubsub_itc_fw_app::decode(view, wire.data(), wire.size(), bytes_consumed, arena, arena_bytes_needed));

    EXPECT_EQ(view.comp_id, "BINCLIENT");
    EXPECT_EQ(bytes_consumed, wire.size());
}

TEST_F(BinaryProtocolTest, LogonAckCarriesTheOutcomeAndItsOptionalText) {
    pubsub_itc_fw_app::LogonAck ack{};
    ack.outcome = pubsub_itc_fw_app::LogonOutcome::DuplicateCompId;
    ack.has_text = true;
    ack.text = "comp_id is already logged on";
    const std::vector<uint8_t> wire = encode_message(ack);

    pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::LogonAckView view{};
    ASSERT_TRUE(pubsub_itc_fw_app::decode(view, wire.data(), wire.size(), bytes_consumed, arena, arena_bytes_needed));

    EXPECT_EQ(view.outcome, pubsub_itc_fw_app::LogonOutcome::DuplicateCompId);
    ASSERT_TRUE(view.has_text);
    EXPECT_EQ(view.text, "comp_id is already logged on");
}

TEST_F(BinaryProtocolTest, LogonAckOmitsTextWhenThereIsNothingToSay) {
    pubsub_itc_fw_app::LogonAck ack{};
    ack.outcome = pubsub_itc_fw_app::LogonOutcome::Accepted;
    const std::vector<uint8_t> wire = encode_message(ack);

    pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::LogonAckView view{};
    ASSERT_TRUE(pubsub_itc_fw_app::decode(view, wire.data(), wire.size(), bytes_consumed, arena, arena_bytes_needed));

    EXPECT_EQ(view.outcome, pubsub_itc_fw_app::LogonOutcome::Accepted);
    EXPECT_FALSE(view.has_text);
}

TEST_F(BinaryProtocolTest, EnvelopeCarriesTheOriginatingGatewayId) {
    const std::vector<uint8_t> order_payload{1, 2, 3, 4};

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
    envelope.payload.data = order_payload.data();
    envelope.payload.size = order_payload.size();
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = 42;
    envelope.has_origin_gateway_id = true;
    envelope.origin_gateway_id = gateway_ids::binary_gateway;
    const std::vector<uint8_t> wire = encode_message(envelope);

    pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    ASSERT_TRUE(pubsub_itc_fw_app::decode(view, wire.data(), wire.size(), bytes_consumed, arena, arena_bytes_needed));

    ASSERT_TRUE(view.has_origin_gateway_id);
    EXPECT_EQ(view.origin_gateway_id, gateway_ids::binary_gateway);
    EXPECT_EQ(view.gateway_session_conn_id, 42);
}

// The compatibility promise made in leader_follower.dsl and relied on by every WAL
// written before the binary gateway existed: origin_gateway_id is optional and
// trailing, so a record without it still decodes, and its absence means the order
// came from the only gateway there was at the time.
TEST_F(BinaryProtocolTest, EnvelopeWithoutAGatewayIdStillDecodesAndMeansTheOrderGateway) {
    const std::vector<uint8_t> order_payload{9, 9, 9};

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
    envelope.payload.data = order_payload.data();
    envelope.payload.size = order_payload.size();
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = 7;
    // Left unset, which reproduces the bytes a pre-binary-gateway writer produced.
    envelope.has_origin_gateway_id = false;
    const std::vector<uint8_t> wire = encode_message(envelope);

    pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    pubsub_itc_fw_app::WalRecordView view{};
    ASSERT_TRUE(pubsub_itc_fw_app::decode(view, wire.data(), wire.size(), bytes_consumed, arena, arena_bytes_needed));

    EXPECT_FALSE(view.has_origin_gateway_id);
    EXPECT_EQ(view.gateway_session_conn_id, 7);
    EXPECT_EQ(gateway_ids::default_when_absent, gateway_ids::order_gateway);
}

// The ids are baked into every WAL record already written, so reassigning one would
// silently misroute historical traffic on replay.
TEST_F(BinaryProtocolTest, GatewayIdsAreDistinctAndStable) {
    EXPECT_EQ(gateway_ids::order_gateway, 1);
    EXPECT_EQ(gateway_ids::binary_gateway, 2);
    EXPECT_NE(gateway_ids::order_gateway, gateway_ids::binary_gateway);
}

} // namespaces
