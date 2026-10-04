// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "LogTailIndex.hpp"

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

constexpr size_t segment_size = 4096;
constexpr int64_t one_second_ns = 1'000'000'000LL;

class LogTailIndexTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("log_tail_index_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The log's helper thread creates the next segment ahead of the writer, so the log is shut
        // down, and the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    // Appends a command's record, as the leader logs it, at the given time.
    void append_command(const std::string& comp_id, int16_t protocol, const std::string& cl_ord_id, int64_t wall_time_ns,
                        int16_t inner_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        const std::vector<uint8_t> payload(40, 0x55);
        pubsub_itc_fw_app::WalRecord record{};
        record.seq_no = ++last_seq_no_;
        record.pdu_id = inner_pdu_id;
        record.payload = pubsub_itc_fw_app::BytesView{payload.data(), payload.size()};
        record.wall_time_ns = wall_time_ns;
        record.has_sender_comp_id = true;
        record.sender_comp_id = comp_id;
        record.has_origin_gateway_id = true;
        record.origin_gateway_id = protocol;
        record.has_cl_ord_id = true;
        record.cl_ord_id = cl_ord_id;
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
        std::vector<uint8_t> buffer(bytes_needed);
        ASSERT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
        wal_->append(record.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, buffer.data(), static_cast<int>(bytes_written), wall_time_ns);
    }

    // One command a second, numbered from 1, so that the log spans many segments.
    void append_commands(int count) {
        for (int number = 1; number <= count; ++number) {
            append_command("MEMBER", gateway_ids::fix_order_gateway, "order-" + std::to_string(number), number * one_second_ns);
        }
    }

    std::string dir_;
    std::optional<pubsub_itc_fw::Wal> wal_;
    int64_t last_seq_no_{0};
};

} // namespaces

TEST_F(LogTailIndexTest, ARecentCommandIsFoundAndOneNeverLoggedIsNot) {
    append_commands(500);
    sequencer::LogTailIndex index(dir_);
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-500", 499 * one_second_ns));
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-501", 499 * one_second_ns));
}

TEST_F(LogTailIndexTest, TheCompIdAndTheProtocolMustBothMatch) {
    append_commands(10);
    sequencer::LogTailIndex index(dir_);
    EXPECT_FALSE(index.holds("OTHER", gateway_ids::fix_order_gateway, "order-5", std::numeric_limits<int64_t>::min()));
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::binary_order_gateway, "order-5", std::numeric_limits<int64_t>::min()));
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-5", std::numeric_limits<int64_t>::min()));
}

// Only as far back as the command's own time, less the margin for the clocks, is read. A command
// older than that is not looked for, because a command sent again cannot have been logged before it
// was first sent.
TEST_F(LogTailIndexTest, OnlyAsMuchOfTheLogIsReadAsTheCommandsTimeRequires) {
    append_commands(500);
    sequencer::LogTailIndex index(dir_);
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-499", 499 * one_second_ns));
    const int64_t read_for_a_recent_command = index.records_read();
    EXPECT_LT(read_for_a_recent_command, 500);
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-1", 499 * one_second_ns)) << "order-1 is older than the time given";
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-1", 1 * one_second_ns));
    EXPECT_EQ(index.records_read(), 500) << "every record read once, and none twice";
}

TEST_F(LogTailIndexTest, ACommandLoggedAfterTheIndexWasBuiltIsFound) {
    append_commands(100);
    sequencer::LogTailIndex index(dir_);
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "later", 100 * one_second_ns));
    append_command("MEMBER", gateway_ids::fix_order_gateway, "later", 101 * one_second_ns);
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "later", 100 * one_second_ns));
}

TEST_F(LogTailIndexTest, AReportIsNotACommand) {
    append_command("MEMBER", gateway_ids::fix_order_gateway, "order-1", one_second_ns,
                   static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport));
    sequencer::LogTailIndex index(dir_);
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-1", std::numeric_limits<int64_t>::min()));
}

TEST_F(LogTailIndexTest, AnEmptyLogHoldsNothing) {
    sequencer::LogTailIndex index(dir_);
    EXPECT_FALSE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-1", std::numeric_limits<int64_t>::min()));
    append_command("MEMBER", gateway_ids::fix_order_gateway, "order-1", one_second_ns);
    EXPECT_TRUE(index.holds("MEMBER", gateway_ids::fix_order_gateway, "order-1", std::numeric_limits<int64_t>::min()));
}
