// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_codec/FixMessageWriter.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixChecksum.hpp>
#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace {

using fix_codec::FixMessageReader;
using fix_codec::FixMessageWriter;
namespace tag = fix_codec::tag;

TEST(FixMessageWriterTest, WritesHeaderBodyAndTrailer) {
    char buffer[128];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::Heartbeat);
    writer.push_back_field(tag::SenderCompID, std::string_view("GATEWAY"));
    writer.push_back_field(tag::TargetCompID, std::string_view("CLIENT"));
    writer.push_back_field(tag::MsgSeqNum, 1);
    const std::string_view wire = writer.finish();

    ASSERT_FALSE(writer.overflowed());
    ASSERT_FALSE(wire.empty());
    // The message opens with BeginString then BodyLength, and closes with Checksum.
    EXPECT_EQ(wire.substr(0, 13), std::string_view("8=FIXT.1.1\x01"
                                                   "9="));
    EXPECT_EQ(wire.substr(wire.size() - 1, 1), std::string_view("\x01"));
    EXPECT_NE(wire.find("\x01"
                        "10="),
              std::string_view::npos);
}

TEST(FixMessageWriterTest, ProducesAReadableSelfConsistentMessage) {
    char buffer[128];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::NewOrderSingle);
    writer.push_back_field(tag::ClOrdID, std::string_view("A-1"));
    writer.push_back_field(tag::OrderQty, 250);
    writer.push_back_field(tag::Side, '2');
    const std::string_view wire = writer.finish();

    // The reader accepts it: BodyLength and Checksum were computed correctly.
    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    EXPECT_EQ(reader.msg_type(), "D");
    EXPECT_EQ(reader.find(tag::ClOrdID).as_string_view(), "A-1");
    EXPECT_EQ(reader.find(tag::OrderQty).as_int(), 250);
    EXPECT_EQ(reader.find(tag::Side).as_char(), '2');
}

TEST(FixMessageWriterTest, BodyLengthCountsOnlyTheBody) {
    char buffer[128];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::Heartbeat);
    const std::string_view wire = writer.finish();

    // Body is everything between the tag 9 SOH and the "10=" field.
    const size_t body_start = wire.find('\x01', wire.find("9=")) + 1;
    const size_t checksum_start = wire.find("10=");
    const size_t body_length = checksum_start - body_start;

    const FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    EXPECT_EQ(reader.find(tag::BodyLength).as_int(), static_cast<int>(body_length));
}

TEST(FixMessageWriterTest, HonoursACustomBeginString) {
    char buffer[128];
    FixMessageWriter writer(buffer, sizeof(buffer), "FIX.4.2");
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::Heartbeat);
    const std::string_view wire = writer.finish();

    ASSERT_FALSE(wire.empty());
    EXPECT_EQ(wire.substr(0, 10), std::string_view("8=FIX.4.2\x01"));
    EXPECT_EQ(FixMessageReader(wire).find(tag::BeginString).as_string_view(), "FIX.4.2");
}

TEST(FixMessageWriterTest, ReportsOverflowOnTooSmallBuffer) {
    char buffer[16];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::SenderCompID, std::string_view("A-VERY-LONG-COMP-ID-VALUE"));
    const std::string_view wire = writer.finish();

    EXPECT_TRUE(writer.overflowed());
    EXPECT_TRUE(wire.empty());
}

} // namespaces
