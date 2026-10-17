// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "PublisherRecordFromWal.hpp"

#include <leader_follower.hpp>

namespace {

// The bytes of a WalRecord envelope numbered @p seq_no, as the sequencer writes one into its WAL.
std::vector<uint8_t> encoded_envelope(int64_t seq_no) {
    const std::vector<uint8_t> message(40, 0x55);
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = seq_no;
    record.pdu_id = 1;
    record.payload = pubsub_itc_fw_app::BytesView{message.data(), message.size()};
    record.wall_time_ns = seq_no;
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

pubsub_itc_fw::WalEntryContents contents_of(int16_t pdu_id, const std::vector<uint8_t>& bytes) {
    pubsub_itc_fw::WalEntryContents contents{};
    contents.pdu_id = pdu_id;
    contents.bytes = bytes.data();
    contents.size = bytes.size();
    return contents;
}

} // un-named namespace

TEST(PublisherRecordFromWalTest, AnEnvelopeIsSentByteForByteAsAWalRecord) {
    const std::vector<uint8_t> envelope = encoded_envelope(12);
    std::vector<uint8_t> arena(64 * 1024);
    std::vector<uint8_t> out;
    const pubsub_itc_fw::ConvertedRecord converted =
        sequencer::publisher_record_from_wal(contents_of(pubsub_itc_fw_app::WalRecord::message_pdu_id, envelope), arena, out);
    EXPECT_EQ(converted.outcome, pubsub_itc_fw::ConvertedRecord::Outcome::Send);
    EXPECT_EQ(converted.pdu_id, pubsub_itc_fw_app::WalRecord::message_pdu_id);
    EXPECT_EQ(out, envelope);
}

TEST(PublisherRecordFromWalTest, ARecordThatIsNotAnEnvelopeIsDamagedNotPassedOver) {
    const std::vector<uint8_t> envelope = encoded_envelope(12);
    std::vector<uint8_t> arena(64 * 1024);
    std::vector<uint8_t> out;
    const pubsub_itc_fw::ConvertedRecord converted = sequencer::publisher_record_from_wal(contents_of(1, envelope), arena, out);
    EXPECT_EQ(converted.outcome, pubsub_itc_fw::ConvertedRecord::Outcome::Damaged)
        << "passing it over would leave a gap in the publisher's numbering, which it would ask about for ever";
}

TEST(PublisherRecordFromWalTest, AnEnvelopeThatDoesNotDecodeIsDamaged) {
    const std::vector<uint8_t> garbage = {1, 2, 3};
    std::vector<uint8_t> arena(64 * 1024);
    std::vector<uint8_t> out;
    const pubsub_itc_fw::ConvertedRecord converted =
        sequencer::publisher_record_from_wal(contents_of(pubsub_itc_fw_app::WalRecord::message_pdu_id, garbage), arena, out);
    EXPECT_EQ(converted.outcome, pubsub_itc_fw::ConvertedRecord::Outcome::Damaged);
}
