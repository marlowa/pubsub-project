// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "EngineCatchUpFromWal.hpp"
#include "GatewayIds.hpp"

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

constexpr size_t segment_size = 4096;
constexpr int16_t new_order = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
constexpr int16_t report = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
constexpr int32_t trading_date = 20261025;

class EngineCatchUpFromWalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("engine_catch_up_from_wal_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The log's helper thread creates the next segment ahead of the writer, so the log is shut down,
        // and the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    // Appends a WalRecord envelope carrying a message of type @p inner_pdu_id.
    void append(int16_t inner_pdu_id) {
        const std::vector<uint8_t> payload(40, 0x55);
        pubsub_itc_fw_app::WalRecord record{};
        record.seq_no = ++last_seq_no_;
        record.pdu_id = inner_pdu_id;
        record.payload = pubsub_itc_fw_app::BytesView{payload.data(), payload.size()};
        record.wall_time_ns = last_seq_no_;
        record.has_sender_comp_id = true;
        record.sender_comp_id = "MEMBER";
        record.has_origin_gateway_id = true;
        record.origin_gateway_id = gateway_ids::fix_order_gateway;
        record.has_gateway_instance_id = true;
        record.gateway_instance_id = 2;
        record.has_trading_date = true;
        record.trading_date = trading_date;
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
        std::vector<uint8_t> buffer(bytes_needed);
        ASSERT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
        wal_->append(record.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, buffer.data(), static_cast<int>(bytes_written), last_seq_no_, 0);
    }

    // Appends a record that says it is a WalRecord envelope but is too short to decode as one: its checksum
    // is sound, so only the decoding finds it damaged.
    void append_damaged() {
        const uint8_t garbage[3] = {1, 2, 3};
        ++last_seq_no_;
        wal_->append(last_seq_no_, pubsub_itc_fw_app::WalRecord::message_pdu_id, garbage, 3, last_seq_no_, 0);
    }

    sequencer::EngineCatchUpFromWal catch_up(int64_t after_seq_no, std::vector<int64_t>& sent) {
        std::vector<uint8_t> arena(64 * 1024);
        return sequencer::engine_catch_up_from_wal(dir_, wal_->scan_start_for(after_seq_no + 1), after_seq_no, last_seq_no_, arena,
                                                   [&sent](int64_t seq_no, const pubsub_itc_fw_app::WalRecord& /*envelope*/) { sent.push_back(seq_no); });
    }

    std::string dir_;
    std::optional<pubsub_itc_fw::Wal> wal_;
    int64_t last_seq_no_{0};
};

} // un-named namespace

TEST_F(EngineCatchUpFromWalTest, EveryOrderAfterThePositionIsSentAndOtherRecordsAreNot) {
    append(new_order); // 1
    append(report);    // 2
    append(new_order); // 3
    append(new_order); // 4
    std::vector<int64_t> sent;
    const sequencer::EngineCatchUpFromWal result = catch_up(1, sent);
    EXPECT_EQ(sent, (std::vector<int64_t>{3, 4}));
    EXPECT_EQ(result.records_sent, 2);
    EXPECT_EQ(result.damaged_seq_no, 0);
}

// The engine applies a command sent from the WAL as it applies one sent live, so the envelope it is sent must carry every
// field the engine reads from it: without the trading date, for one, the engine refuses the command, because the keys of
// its identifiers are derived from it.
TEST_F(EngineCatchUpFromWalTest, AnOrderIsSentWithEveryFieldTheEngineReads) {
    append(new_order); // 1
    std::vector<uint8_t> arena(64 * 1024);
    std::vector<pubsub_itc_fw_app::WalRecord> sent;
    std::vector<std::string> comp_ids;
    static_cast<void>(sequencer::engine_catch_up_from_wal(dir_, wal_->scan_start_for(1), 0, last_seq_no_, arena,
                                                          [&sent, &comp_ids](int64_t /*seq_no*/, const pubsub_itc_fw_app::WalRecord& envelope) {
                                                              sent.push_back(envelope);
                                                              comp_ids.emplace_back(envelope.sender_comp_id);
                                                          }));
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].seq_no, 1);
    EXPECT_EQ(sent[0].pdu_id, new_order);
    EXPECT_EQ(sent[0].wall_time_ns, 1);
    EXPECT_EQ(sent[0].payload.size, 40U);
    EXPECT_EQ(comp_ids[0], "MEMBER");
    EXPECT_TRUE(sent[0].has_origin_gateway_id);
    EXPECT_EQ(sent[0].origin_gateway_id, gateway_ids::fix_order_gateway);
    EXPECT_TRUE(sent[0].has_gateway_instance_id);
    EXPECT_EQ(sent[0].gateway_instance_id, 2);
    EXPECT_TRUE(sent[0].has_trading_date);
    EXPECT_EQ(sent[0].trading_date, trading_date);
}

TEST_F(EngineCatchUpFromWalTest, NothingIsSentAfterADamagedRecordAndItsNumberIsReturned) {
    append(new_order); // 1
    append(new_order); // 2
    append_damaged();  // 3
    append(new_order); // 4
    append(new_order); // 5
    std::vector<int64_t> sent;
    const sequencer::EngineCatchUpFromWal result = catch_up(0, sent);
    EXPECT_EQ(sent, (std::vector<int64_t>{1, 2})) << "an order after a record that could not be read was sent";
    EXPECT_EQ(result.damaged_seq_no, 3);
}

TEST_F(EngineCatchUpFromWalTest, AnEngineAlreadyPastADamagedRecordIsSentTheRest) {
    append(new_order); // 1
    append_damaged();  // 2
    append(new_order); // 3
    append(new_order); // 4
    std::vector<int64_t> sent;
    const sequencer::EngineCatchUpFromWal result = catch_up(2, sent);
    EXPECT_EQ(sent, (std::vector<int64_t>{3, 4}));
    EXPECT_EQ(result.damaged_seq_no, 0);
}
