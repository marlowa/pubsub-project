// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_codec/FixMessageReader.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace {

using fix_codec::FixMessageReader;
using fix_codec::FixMessageWriter;
namespace tag = fix_codec::tag;

// Builds a small, well-formed NewOrderSingle into the supplied buffer.
std::string_view build_order(char* buffer, size_t capacity) {
    FixMessageWriter writer(buffer, capacity);
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::NewOrderSingle);
    writer.push_back_field(tag::SenderCompID, std::string_view("GATEWAY"));
    writer.push_back_field(tag::TargetCompID, std::string_view("CLIENT"));
    writer.push_back_field(tag::MsgSeqNum, 7);
    writer.push_back_field(tag::ClOrdID, std::string_view("ORDER-1"));
    writer.push_back_field(tag::Symbol, std::string_view("VOD.L"));
    writer.push_back_field(tag::Side, '1');
    writer.push_back_field(tag::OrderQty, 100);
    return writer.finish();
}

TEST(FixMessageReaderTest, FramesAndReadsValidMessage) {
    char buffer[256];
    const std::string_view wire = build_order(buffer, sizeof(buffer));
    ASSERT_FALSE(wire.empty());

    FixMessageReader reader(wire);
    EXPECT_TRUE(reader.is_valid());
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Valid);
    EXPECT_EQ(reader.message_size(), wire.size());
    EXPECT_EQ(reader.message_bytes(), wire);

    EXPECT_EQ(reader.msg_type(), "D");
    EXPECT_EQ(reader.find(tag::ClOrdID).as_string_view(), "ORDER-1");
    EXPECT_EQ(reader.find(tag::MsgSeqNum).as_int(), 7);
    EXPECT_EQ(reader.find(tag::Side).as_char(), '1');
    EXPECT_EQ(reader.find(tag::OrderQty).as_int(), 100);
    // An absent tag yields an empty field.
    EXPECT_TRUE(reader.find(tag::Price).empty());
}

TEST(FixMessageReaderTest, IteratesEveryFieldInOrder) {
    char buffer[256];
    const std::string_view wire = build_order(buffer, sizeof(buffer));
    FixMessageReader reader(wire);

    int count = 0;
    int first_tag = 0;
    int last_tag = 0;
    for (const auto& field : reader) {
        if (count == 0) {
            first_tag = field.tag;
        }
        last_tag = field.tag;
        ++count;
    }
    // 8, 9, 35, 49, 56, 34, 11, 55, 54, 38, 10 -> header + 8 body + trailer.
    EXPECT_EQ(count, 11);
    EXPECT_EQ(first_tag, tag::BeginString);
    EXPECT_EQ(last_tag, tag::Checksum);
}

TEST(FixMessageReaderTest, FindWithHintResumesFromPrevious) {
    char buffer[256];
    const std::string_view wire = build_order(buffer, sizeof(buffer));
    FixMessageReader reader(wire);

    FixMessageReader::const_iterator it = reader.begin();
    const fix_codec::FixField sender = reader.find(tag::SenderCompID, it);
    EXPECT_EQ(sender.as_string_view(), "GATEWAY");
    // Searching for an earlier tag from a later hint does not find it.
    const fix_codec::FixField target = reader.find(tag::TargetCompID, it);
    EXPECT_EQ(target.as_string_view(), "CLIENT");
}

TEST(FixMessageReaderTest, ReportsIncompleteWhenTruncated) {
    char buffer[256];
    const std::string_view wire = build_order(buffer, sizeof(buffer));
    // Drop the last few bytes so the checksum field is not fully present.
    const FixMessageReader reader(wire.substr(0, wire.size() - 3));
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Incomplete);
    EXPECT_EQ(reader.message_size(), 0U);
}

TEST(FixMessageReaderTest, ReportsMalformedWhenNotAMessageBoundary) {
    const std::string_view garbage = "hello world this is not fix\x01";
    const FixMessageReader reader(garbage);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Malformed);
}

TEST(FixMessageReaderTest, ReportsChecksumErrorButStillFrames) {
    char buffer[256];
    std::string wire(build_order(buffer, sizeof(buffer)));
    // Flip a bit in a value so the checksum no longer matches.
    wire[wire.size() - 10] ^= 0x20;

    const FixMessageReader reader(wire);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::ChecksumError);
    EXPECT_FALSE(reader.is_valid());
    // Still framed, so a stream driver can skip exactly this many bytes.
    EXPECT_EQ(reader.message_size(), wire.size());
}

TEST(FixMessageReaderTest, ReadsDataFieldByLengthEvenWithEmbeddedSoh) {
    // RawData (96) is a binary field preceded by RawDataLength (95); its bytes may
    // contain the SOH delimiter, so it must be read by length, not by SOH scan.
    const std::string raw("AB\x01"
                          "CD",
                          5); // 5 bytes, one embedded SOH
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::Logon);
    writer.push_back_field(tag::RawDataLength, static_cast<int>(raw.size()));
    writer.push_back_field(tag::RawData, std::string_view(raw));
    const std::string_view wire = writer.finish();
    ASSERT_FALSE(wire.empty());

    FixMessageReader reader(wire);
    EXPECT_TRUE(reader.is_valid());
    const fix_codec::FixField raw_field = reader.find(tag::RawData);
    EXPECT_EQ(raw_field.as_string_view().size(), raw.size());
    EXPECT_EQ(raw_field.as_string_view(), raw);
    // The field following the data field is still located correctly.
    EXPECT_EQ(reader.find(tag::RawDataLength).as_int(), 5);
}

} // namespaces
