// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "FullyReportedThrough.hpp"

#include <fix_orders.hpp>
#include <leader_follower.hpp>

namespace {

// The bytes of a WalRecord envelope carrying a message of type @p inner_pdu_id, produced by command @p produced_by and
// answering command @p answers, or no command when @p answers is negative.
std::vector<uint8_t> encoded_envelope(int16_t inner_pdu_id, int64_t produced_by, int64_t answers) {
    const std::vector<uint8_t> message(40, 0x55);
    pubsub_itc_fw_app::WalRecord record{};
    record.seq_no = 100;
    record.pdu_id = inner_pdu_id;
    record.payload = pubsub_itc_fw_app::BytesView{message.data(), message.size()};
    record.wall_time_ns = 1;
    record.has_answers_seq_no = answers >= 0;
    record.answers_seq_no = answers >= 0 ? answers : 0;
    record.produced_by_seq_no = produced_by;
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    static_cast<void>(pubsub_itc_fw_app::encode(record, nullptr, 0, bytes_written, bytes_needed));
    std::vector<uint8_t> buffer(bytes_needed);
    EXPECT_TRUE(pubsub_itc_fw_app::encode(record, buffer.data(), buffer.size(), bytes_written, bytes_needed));
    buffer.resize(bytes_written);
    return buffer;
}

constexpr auto report = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
constexpr auto new_order = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle);
constexpr int64_t no_command = -1;

std::optional<int64_t> read(int16_t pdu_id, const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> arena(64 * 1024);
    return sequencer::fully_reported_through(pdu_id, bytes.data(), bytes.size(), arena);
}

} // un-named namespace

// A report produced by command 41 means every report of every command before it is in the WAL, and 41 may have more to
// come.
TEST(FullyReportedThroughTest, AReportProducedByACommandSaysTheWalHoldsEveryReportOfTheCommandsBeforeIt) {
    EXPECT_EQ(read(pubsub_itc_fw_app::WalRecord::message_pdu_id, encoded_envelope(report, 41, 41)), 40);
}

TEST(FullyReportedThroughTest, ACommandSaysNothing) {
    EXPECT_FALSE(read(pubsub_itc_fw_app::WalRecord::message_pdu_id, encoded_envelope(new_order, 0, 41)).has_value());
}

// A cancel of a session's orders answers no member's command, but was produced by applying command 41, so it says the
// same as any other report of 41 (docs/bug_list.md, BUG-0263).
TEST(FullyReportedThroughTest, AReportThatAnswersNoCommandSaysWhatItsProducingCommandSays) {
    EXPECT_EQ(read(pubsub_itc_fw_app::WalRecord::message_pdu_id, encoded_envelope(report, 41, no_command)), 40);
}

// A report with no producing command, which no engine sends, says nothing.
TEST(FullyReportedThroughTest, AReportWithNoProducingCommandSaysNothing) {
    EXPECT_FALSE(read(pubsub_itc_fw_app::WalRecord::message_pdu_id, encoded_envelope(report, 0, 41)).has_value());
}

TEST(FullyReportedThroughTest, ARecordThatIsNotAnEnvelopeOrDoesNotDecodeSaysNothing) {
    const std::vector<uint8_t> envelope = encoded_envelope(report, 41, 41);
    EXPECT_FALSE(read(1, envelope).has_value());
    const std::vector<uint8_t> truncated(envelope.begin(), envelope.begin() + 5);
    EXPECT_FALSE(read(pubsub_itc_fw_app::WalRecord::message_pdu_id, truncated).has_value());
}
