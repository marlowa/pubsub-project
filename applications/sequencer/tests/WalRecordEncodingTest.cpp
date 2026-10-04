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

TEST(WalRecordEncodingTest, ARecordWrittenBeforeTheEpochFieldExistedStillDecodes) {
    // A record encoded without the field ends one byte earlier than one that says it is absent: the
    // presence byte. Removing that byte gives exactly the bytes a record written before the field was
    // added to the message holds, as the records already in a write-ahead log do.
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = 42;
    record.has_leader_epoch = false;
    std::vector<uint8_t> old_form = encoded(record);
    ASSERT_EQ(old_form.back(), 0);
    old_form.pop_back();
    pubsub_itc_fw_app::WalRecordView view{};
    ASSERT_TRUE(decoded(old_form, view));
    EXPECT_EQ(view.seq_no, 42);
    EXPECT_FALSE(view.has_leader_epoch);
}
