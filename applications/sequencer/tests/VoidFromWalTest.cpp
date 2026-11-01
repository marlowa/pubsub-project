// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "VoidFromWal.hpp"
#include "VoidedCommandList.hpp"

#include <leader_follower.hpp>

namespace {

using sequencer::VoidFromWal;

template <typename Message> std::vector<uint8_t> encoded(const Message& message) {
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(message, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(message, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

std::vector<uint8_t> void_command(int64_t first, int64_t last, bool new_orders_only) {
    pubsub_itc_fw_app::VoidCommands command{};
    command.first_seq_no = first;
    command.last_seq_no = last;
    command.new_orders_only = new_orders_only;
    command.reason = pubsub_itc_fw_app::VoidReason::HeldWhenTradingHalted;
    return encoded(command);
}

// The bytes of a WalRecord envelope numbered @p seq_no carrying @p command, as the sequencer writes one into its WAL.
std::vector<uint8_t> envelope_of(int64_t seq_no, int16_t inner_pdu_id, const std::vector<uint8_t>& command) {
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = seq_no;
    record.pdu_id = inner_pdu_id;
    record.payload = pubsub_itc_fw_app::BytesView{command.data(), command.size()};
    record.wall_time_ns = seq_no;
    return encoded(record);
}

pubsub_itc_fw::WalEntryContents contents_of(const std::vector<uint8_t>& bytes) {
    pubsub_itc_fw::WalEntryContents contents{};
    contents.pdu_id = pubsub_itc_fw_app::WalRecord::message_pdu_id;
    contents.bytes = bytes.data();
    contents.size = bytes.size();
    return contents;
}

constexpr auto void_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id);

} // un-named namespace

TEST(VoidFromWalTest, AVoidCommandsRecordGivesItsVoidAtTheRecordsPosition) {
    const std::vector<uint8_t> record = envelope_of(40, void_pdu_id, void_command(10, 12, true));
    std::vector<uint8_t> arena(64 * 1024);
    fix_common::VoidedRange voided{};
    ASSERT_EQ(sequencer::void_from_wal(40, contents_of(record), arena, voided), VoidFromWal::Void);
    EXPECT_EQ(voided.void_seq_no, 40);
    EXPECT_EQ(voided.first_seq_no, 10);
    EXPECT_EQ(voided.last_seq_no, 12);
    EXPECT_TRUE(voided.new_orders_only);
    EXPECT_EQ(voided.reason, pubsub_itc_fw_app::VoidReason::HeldWhenTradingHalted);
}

TEST(VoidFromWalTest, ARecordHoldingSomethingElseIsNotAVoid) {
    const std::vector<uint8_t> order(30, 0x41);
    const std::vector<uint8_t> record = envelope_of(7, 1, order);
    std::vector<uint8_t> arena(64 * 1024);
    fix_common::VoidedRange voided{};
    EXPECT_EQ(sequencer::void_from_wal(7, contents_of(record), arena, voided), VoidFromWal::NotAVoid);
}

TEST(VoidFromWalTest, ARecordThatDoesNotDecodeIsSaidNotToDecode) {
    const std::vector<uint8_t> garbage = {1, 2, 3};
    std::vector<uint8_t> arena(64 * 1024);
    fix_common::VoidedRange voided{};
    EXPECT_EQ(sequencer::void_from_wal(7, contents_of(garbage), arena, voided), VoidFromWal::NotDecodable);
}

// The sequencer keeps its list of voids as it writes each command, and rebuilds it from the WAL when it opens: the two
// must be the same list, or a sequencer that restarts would tell the engines of different voids.
TEST(VoidFromWalTest, TheListRebuiltFromTheWalIsTheListKeptWhileWritingIt) {
    struct Written {
        int64_t seq_no;
        int16_t inner_pdu_id;
        std::vector<uint8_t> command;
    };
    const std::vector<Written> written = {
        {1, 1, std::vector<uint8_t>(20, 0x41)},
        {2, void_pdu_id, void_command(1, 1, false)},
        {3, 1, std::vector<uint8_t>(20, 0x42)},
        {4, void_pdu_id, void_command(3, 3, true)},
    };
    std::vector<uint8_t> arena(64 * 1024);
    fix_common::VoidedCommandList kept_while_writing;
    fix_common::VoidedCommandList rebuilt;
    for (const Written& record : written) {
        fix_common::VoidedRange voided{};
        if (sequencer::void_from_command(record.seq_no, record.inner_pdu_id, record.command.data(), record.command.size(), arena, voided) ==
            VoidFromWal::Void) {
            kept_while_writing.add(voided);
        }
        const std::vector<uint8_t> stored = envelope_of(record.seq_no, record.inner_pdu_id, record.command);
        if (sequencer::void_from_wal(record.seq_no, contents_of(stored), arena, voided) == VoidFromWal::Void) {
            rebuilt.add(voided);
        }
    }

    ASSERT_EQ(rebuilt.size(), 2U);
    ASSERT_EQ(rebuilt.size(), kept_while_writing.size());
    for (size_t index = 0; index < rebuilt.size(); ++index) {
        const fix_common::VoidedRange& from_wal = rebuilt.voids()[index];
        const fix_common::VoidedRange& from_writing = kept_while_writing.voids()[index];
        EXPECT_EQ(from_wal.void_seq_no, from_writing.void_seq_no) << "void " << index;
        EXPECT_EQ(from_wal.first_seq_no, from_writing.first_seq_no) << "void " << index;
        EXPECT_EQ(from_wal.last_seq_no, from_writing.last_seq_no) << "void " << index;
        EXPECT_EQ(from_wal.new_orders_only, from_writing.new_orders_only) << "void " << index;
        EXPECT_EQ(from_wal.reason, from_writing.reason) << "void " << index;
    }
}

// Only a member's new order or cancel may be voided: a command the sequencer wrote itself, or a report, may not.
TEST(VoidFromWalTest, OnlyAMembersNewOrderOrCancelIsAMembersCommand) {
    std::vector<uint8_t> arena(64 * 1024);
    const std::vector<uint8_t> command(20, 0x41);
    const auto kind_of = [&](int16_t inner_pdu_id) {
        const std::vector<uint8_t> record = envelope_of(5, inner_pdu_id, command);
        return sequencer::command_in_record(contents_of(record), arena);
    };
    EXPECT_EQ(kind_of(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)), sequencer::CommandInRecord::MembersCommand);
    EXPECT_EQ(kind_of(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)), sequencer::CommandInRecord::MembersCommand);
    EXPECT_EQ(kind_of(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)), sequencer::CommandInRecord::OtherRecord);
    EXPECT_EQ(kind_of(static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)), sequencer::CommandInRecord::OtherRecord);
    EXPECT_EQ(kind_of(static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)), sequencer::CommandInRecord::OtherRecord);
    EXPECT_EQ(kind_of(void_pdu_id), sequencer::CommandInRecord::OtherRecord);
    const std::vector<uint8_t> garbage = {1, 2, 3};
    EXPECT_EQ(sequencer::command_in_record(contents_of(garbage), arena), sequencer::CommandInRecord::NotDecodable);
}
