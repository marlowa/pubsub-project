// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <leader_follower.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>

namespace {

std::vector<uint8_t> encoded(const pubsub_itc_fw_app::WalRecord& record) {
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    const bool fits_in_nothing = pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed);
    EXPECT_FALSE(fits_in_nothing) << "measuring with no buffer reports the size needed and writes nothing";
    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

bool decoded(const std::vector<uint8_t>& buffer, pubsub_itc_fw_app::WalRecordView& view) {
    std::array<uint8_t, 4096> arena_buffer{};
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    return pubsub_itc_fw_app::decode(view, buffer.data(), buffer.size(), bytes_consumed, arena, arena_bytes_needed);
}

} // un-named namespace

TEST(WalRecordEncodingTest, ARecordCarriesTheEpochOfTheLeadershipThatWroteIt) {
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = 42;
    record.has_leader_epoch = true;
    record.leader_epoch = 9;
    pubsub_itc_fw_app::WalRecordView view{};
    ASSERT_TRUE(decoded(encoded(record), view));
    EXPECT_TRUE(view.has_leader_epoch);
    EXPECT_EQ(view.leader_epoch, 9);
}

// A report carries the command that produced it as well as the one it answers, which for a cancel of a session's orders
// is none (docs/bug_list.md, BUG-0263).
TEST(WalRecordEncodingTest, AReportCarriesTheCommandThatProducedItAsWellAsTheOneItAnswers) {
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = 42;
    record.has_answers_seq_no = false;
    record.produced_by_seq_no = 41;
    pubsub_itc_fw_app::WalRecordView view{};
    ASSERT_TRUE(decoded(encoded(record), view));
    EXPECT_FALSE(view.has_answers_seq_no);
    EXPECT_EQ(view.produced_by_seq_no, 41);
}
