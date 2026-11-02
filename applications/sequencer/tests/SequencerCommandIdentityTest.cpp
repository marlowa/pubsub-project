// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "SequencerCommandIdentity.hpp"

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>

namespace {

constexpr int16_t new_order = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
constexpr int16_t cancel = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
constexpr int16_t report = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);

using Buffer = std::array<char, pubsub_itc_fw::CommandIdentityInterface::max_identity_size>;

std::string identity(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id) {
    Buffer buffer{};
    const size_t size = sequencer::write_command_identity(comp_id, protocol, cl_ord_id, buffer.data(), buffer.size());
    return std::string(buffer.data(), size);
}

// A WAL record's bytes as the leader writes a member's command: a WalRecord envelope.
std::vector<uint8_t> envelope(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id, int16_t inner_pdu_id) {
    const std::vector<uint8_t> payload(40, 0x55);
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = 1;
    record.pdu_id = inner_pdu_id;
    record.payload = pubsub_itc_fw_app::BytesView{payload.data(), payload.size()};
    record.wall_time_ns = 1;
    record.has_sender_comp_id = !comp_id.empty();
    record.sender_comp_id = comp_id;
    record.has_origin_gateway_id = true;
    record.origin_gateway_id = protocol;
    record.has_cl_ord_id = !cl_ord_id.empty();
    record.cl_ord_id = cl_ord_id;
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

std::string identity_of_record(const std::vector<uint8_t>& bytes, int16_t pdu_id = pubsub_itc_fw_app::WalRecord::message_pdu_id) {
    const sequencer::SequencerCommandIdentity reader;
    std::vector<uint8_t> scratch(pubsub_itc_fw::CommandIdentityInterface::scratch_size);
    Buffer buffer{};
    const size_t size = reader.identity_of(pubsub_itc_fw::WalEntryContents{1, 0, pdu_id, bytes.data(), bytes.size()}, scratch.data(), buffer.data());
    return std::string(buffer.data(), size);
}

} // namespaces

TEST(SequencerCommandIdentityTest, EachOfTheThreeValuesChangesTheIdentity) {
    const std::string base = identity("MEMBER1", gateway_ids::fix_order_gateway, "order-1");
    EXPECT_FALSE(base.empty());
    EXPECT_NE(base, identity("MEMBER2", gateway_ids::fix_order_gateway, "order-1"));
    EXPECT_NE(base, identity("MEMBER1", gateway_ids::binary_order_gateway, "order-1"));
    EXPECT_NE(base, identity("MEMBER1", gateway_ids::fix_order_gateway, "order-2"));
    EXPECT_EQ(base, identity("MEMBER1", gateway_ids::fix_order_gateway, "order-1"));
}

// The comp id is written with its length, so moving characters between it and the ClOrdID gives a different
// identity, whatever the characters.
TEST(SequencerCommandIdentityTest, MovingCharactersBetweenTheCompIdAndTheClOrdIdChangesTheIdentity) {
    EXPECT_NE(identity("AB", 1, "C"), identity("A", 1, "BC"));
    EXPECT_NE(identity(std::string_view("A\0B", 3), 1, "C"), identity("A", 1, std::string_view("\0BC", 3)));
}

TEST(SequencerCommandIdentityTest, ACommandThatDoesNotSayWhoseItIsHasNoIdentity) {
    EXPECT_TRUE(identity("", gateway_ids::fix_order_gateway, "order-1").empty());
    EXPECT_TRUE(identity("MEMBER", gateway_ids::fix_order_gateway, "").empty());
}

TEST(SequencerCommandIdentityTest, AnIdentityTooLargeForTheBufferIsNone) {
    EXPECT_TRUE(identity("MEMBER", 1, std::string(2000, 'x')).empty());
}

TEST(SequencerCommandIdentityTest, NewOrdersAndCancelsInTheWalHaveTheIdentityOfTheirEnvelope) {
    EXPECT_EQ(identity_of_record(envelope("MEMBER", gateway_ids::fix_order_gateway, "order-1", new_order)),
              identity("MEMBER", gateway_ids::fix_order_gateway, "order-1"));
    EXPECT_EQ(identity_of_record(envelope("MEMBER", gateway_ids::binary_order_gateway, "cancel-1", cancel)),
              identity("MEMBER", gateway_ids::binary_order_gateway, "cancel-1"));
}

TEST(SequencerCommandIdentityTest, OtherRecordsHaveNoIdentity) {
    EXPECT_TRUE(identity_of_record(envelope("MEMBER", gateway_ids::fix_order_gateway, "order-1", report)).empty());
    EXPECT_TRUE(identity_of_record(envelope("MEMBER", gateway_ids::fix_order_gateway, "order-1", new_order), 999).empty());
    EXPECT_TRUE(identity_of_record(envelope("", gateway_ids::fix_order_gateway, "order-1", new_order)).empty());
}
