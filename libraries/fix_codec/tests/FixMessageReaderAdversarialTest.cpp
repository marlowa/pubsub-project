// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/fix_dictionary.hpp>

// Adversarial tests for the framing layer (FixMessageReader). These feed broken
// framing and assert two things: the reader degrades correctly (Malformed or
// Incomplete, never a false Valid and never an out-of-bounds read), and it reports
// a specific reason via error() -- different framing faults give different text, so
// a caller is not left with a bare status. Semantic errors (duplicate, missing, and
// wrong-type fields) are the validator's job and live in FixMessageValidatorTest.

namespace {

using fix_codec::FixField;
using fix_codec::FixMessageReader;
using fix_codec::FixMessageWriter;
namespace tag = fix_codec::tag;

constexpr std::string_view non_numeric_body_length = "8=FIXT.1.1\x01"
                                                     "9=abc\x01"
                                                     "35=D\x01"
                                                     "10=000\x01";
constexpr std::string_view negative_body_length = "8=FIXT.1.1\x01"
                                                  "9=-5\x01"
                                                  "35=D\x01"
                                                  "10=000\x01";
constexpr std::string_view understated_body_length = "8=FIXT.1.1\x01"
                                                     "9=2\x01"
                                                     "35=D\x01"
                                                     "10=000\x01";

// A non-numeric BodyLength is not a length: Malformed, and error() says so.
TEST(FixMessageReaderAdversarialTest, NonNumericBodyLengthIsMalformed) {
    const FixMessageReader reader(non_numeric_body_length);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Malformed);
    EXPECT_EQ(reader.message_size(), 0U);
    const std::optional<std::string> error = reader.error();
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("BodyLength"), std::string::npos);
    EXPECT_NE(error->find("not a number"), std::string::npos);
}

// A negative BodyLength is rejected with a distinctly different reason.
TEST(FixMessageReaderAdversarialTest, NegativeBodyLengthIsMalformed) {
    const FixMessageReader reader(negative_body_length);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Malformed);
    const std::optional<std::string> error = reader.error();
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("positive"), std::string::npos);
}

// An understated BodyLength puts the checksum tag where "10=" is not: Malformed,
// and the reason points at the Checksum tag, not at BodyLength.
TEST(FixMessageReaderAdversarialTest, UnderstatedBodyLengthMisalignsChecksumTag) {
    const FixMessageReader reader(understated_body_length);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Malformed);
    const std::optional<std::string> error = reader.error();
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("Checksum"), std::string::npos);
}

// The three framing faults above must each report a different explanation -- a
// single "Malformed" status is not enough to tell them apart.
TEST(FixMessageReaderAdversarialTest, DistinctFaultsGiveDistinctErrorText) {
    const std::optional<std::string> non_numeric = FixMessageReader(non_numeric_body_length).error();
    const std::optional<std::string> negative = FixMessageReader(negative_body_length).error();
    const std::optional<std::string> understated = FixMessageReader(understated_body_length).error();
    ASSERT_TRUE(non_numeric.has_value() && negative.has_value() && understated.has_value());
    EXPECT_NE(*non_numeric, *negative);
    EXPECT_NE(*non_numeric, *understated);
    EXPECT_NE(*negative, *understated);
}

// An overstated BodyLength points the checksum tag beyond the window: the reader
// must wait for more bytes (Incomplete), never read past the end.
TEST(FixMessageReaderAdversarialTest, OverstatedBodyLengthIsIncomplete) {
    const std::string_view wire = "8=FIXT.1.1\x01"
                                  "9=99\x01"
                                  "35=D\x01"
                                  "10=000\x01";
    const FixMessageReader reader(wire);
    EXPECT_EQ(reader.status(), FixMessageReader::Status::Incomplete);
    EXPECT_EQ(reader.message_size(), 0U);
    EXPECT_TRUE(reader.error().has_value());
}

// A DATA length field that overstates its value must not read past the message.
// The reader abandons length-based reading when the declared length overruns the
// window and falls back to scanning for SOH, keeping the value inside bounds.
TEST(FixMessageReaderAdversarialTest, OverstatedDataLengthDoesNotOverRead) {
    const std::string raw("AB\x01"
                          "CD",
                          5); // five bytes, one embedded SOH
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::Logon);
    writer.push_back_field(tag::RawDataLength, 99); // lies: the real length is 5
    writer.push_back_field(tag::RawData, std::string_view(raw));
    const std::string_view wire = writer.finish();
    ASSERT_FALSE(wire.empty());

    FixMessageReader reader(wire);
    EXPECT_TRUE(reader.is_valid());           // framing (BodyLength + Checksum) is self-consistent
    EXPECT_FALSE(reader.error().has_value()); // a valid message has no error text
    const FixField raw_field = reader.find(tag::RawData);
    EXPECT_EQ(raw_field.as_string_view(), "AB"); // truncated at the embedded SOH, not over-read
    EXPECT_LT(raw_field.as_string_view().size(), raw.size());
}

} // namespaces
