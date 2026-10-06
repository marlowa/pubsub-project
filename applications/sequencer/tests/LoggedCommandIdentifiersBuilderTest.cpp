// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "LoggedCommandIdentifiers.hpp"
#include "LoggedCommandIdentifiersBuilder.hpp"

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

constexpr size_t segment_size = 4096;
constexpr int16_t new_order = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
constexpr int16_t cancel = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
constexpr int16_t report = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);

uint64_t id_of(const std::string& cl_ord_id) {
    return sequencer::LoggedCommandIdentifiers::identifier("MEMBER", gateway_ids::fix_order_gateway, cl_ord_id);
}

class LoggedCommandIdentifiersBuilderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("logged_command_identifiers_builder_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The log's helper thread creates the next segment ahead of the writer, so the log is shut
        // down, and the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    void append(const std::string& cl_ord_id, int16_t inner_pdu_id) {
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
        record.has_cl_ord_id = true;
        record.cl_ord_id = cl_ord_id;
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
        std::vector<uint8_t> buffer(bytes_needed);
        ASSERT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
        wal_->append(record.seq_no, pubsub_itc_fw_app::WalRecord::message_pdu_id, buffer.data(), static_cast<int>(bytes_written), last_seq_no_);
    }

    std::vector<std::string> segment_paths() const {
        std::vector<std::string> paths;
        for (const uint64_t segment : wal_->segments_on_disk()) {
            paths.push_back(wal_->segment_path(segment));
        }
        return paths;
    }

    bool wait_until_finished(const sequencer::LoggedCommandIdentifiersBuilder& builder) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!builder.finished()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return true;
    }

    std::string dir_;
    std::optional<pubsub_itc_fw::Wal> wal_;
    int64_t last_seq_no_{0};
};

} // namespaces

TEST(CommandIdentifierTest, NewOrdersAndCancelsHaveAnIdentifierAndOtherRecordsDoNot) {
    EXPECT_TRUE(sequencer::command_identifier("MEMBER", gateway_ids::fix_order_gateway, new_order, "order-1").has_value());
    EXPECT_TRUE(sequencer::command_identifier("MEMBER", gateway_ids::fix_order_gateway, cancel, "cancel-1").has_value());
    EXPECT_FALSE(sequencer::command_identifier("MEMBER", gateway_ids::fix_order_gateway, report, "order-1").has_value());
}

TEST(CommandIdentifierTest, ACommandThatDoesNotSayWhoseItIsHasNoIdentifier) {
    EXPECT_FALSE(sequencer::command_identifier("", gateway_ids::fix_order_gateway, new_order, "order-1").has_value());
    EXPECT_FALSE(sequencer::command_identifier("MEMBER", gateway_ids::fix_order_gateway, new_order, "").has_value());
}

TEST_F(LoggedCommandIdentifiersBuilderTest, TheBuiltRecordHoldsEveryCommandInTheLogAcrossSegments) {
    for (int number = 1; number <= 300; ++number) {
        append("order-" + std::to_string(number), new_order);
    }
    append("cancel-1", cancel);
    ASSERT_GT(wal_->segments_on_disk().size(), 3U) << "the log should span several segments for this to test anything";

    sequencer::LoggedCommandIdentifiersBuilder builder;
    builder.start(10000, segment_paths());
    ASSERT_TRUE(wait_until_finished(builder));
    sequencer::LoggedCommandIdentifiers record = builder.take();

    EXPECT_EQ(record.size(), 301U);
    for (int number = 1; number <= 300; ++number) {
        ASSERT_TRUE(record.may_hold(id_of("order-" + std::to_string(number)))) << "order-" << number;
    }
    EXPECT_TRUE(record.may_hold(id_of("cancel-1")));
    EXPECT_EQ(builder.records_read(), 301);
}

TEST_F(LoggedCommandIdentifiersBuilderTest, RecordsThatAreNotCommandsAreNotRecorded) {
    append("order-1", new_order);
    append("order-2", report);
    sequencer::LoggedCommandIdentifiersBuilder builder;
    builder.start(1000, segment_paths());
    ASSERT_TRUE(wait_until_finished(builder));
    sequencer::LoggedCommandIdentifiers record = builder.take();
    EXPECT_EQ(record.size(), 1U);
    EXPECT_TRUE(record.may_hold(id_of("order-1")));
}

TEST_F(LoggedCommandIdentifiersBuilderTest, NothingIsFinishedBeforeTheLogHasBeenRead) {
    sequencer::LoggedCommandIdentifiersBuilder builder;
    EXPECT_FALSE(builder.finished());
}

TEST_F(LoggedCommandIdentifiersBuilderTest, StoppingABuildThatHasNotFinishedEndsItWithoutARecord) {
    for (int number = 1; number <= 3000; ++number) {
        append("order-" + std::to_string(number), new_order);
    }
    std::vector<std::string> paths = segment_paths();
    // The same segments many times over, so that the build is certainly still going when it is stopped.
    std::vector<std::string> many;
    for (int copy = 0; copy < 200; ++copy) {
        many.insert(many.end(), paths.begin(), paths.end());
    }
    sequencer::LoggedCommandIdentifiersBuilder builder;
    builder.start(100000, std::move(many));
    builder.stop();
    EXPECT_FALSE(builder.finished()) << "a build that was stopped reported itself finished";
}
