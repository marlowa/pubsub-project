// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixMessageValidator.hpp>
#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/FixReject.hpp>
#include <fix_codec/fix_dictionary.hpp>

// Adversarial tests for the validation layer (FixMessageValidator). Each feeds a
// well-framed NewOrderSingle that is semantically wrong -- a required tag removed,
// a tag duplicated, or a value of the wrong type -- and asserts that validation
// rejects it, names the specific offending tag, and gives the right FIX reason.
// These messages all pass framing (is_valid() is true); it is validation that
// must catch them.

namespace {

using fix_codec::FixMessageReader;
using fix_codec::FixMessageValidator;
using fix_codec::FixMessageWriter;
using fix_codec::FixReject;
using fix_codec::RejectReason;
namespace tag = fix_codec::tag;

// Writes the mandatory fields of a conforming NewOrderSingle, optionally skipping
// one tag (to build a message that is missing exactly one required field). The
// writer adds BeginString/BodyLength/Checksum itself, so this covers only the
// body and the settable header fields.
void push_mandatory(FixMessageWriter& writer, int skip = 0) {
    if (skip != tag::MsgType) {
        writer.push_back_field(tag::MsgType, fix_codec::msg_type::NewOrderSingle);
    }
    if (skip != tag::SenderCompID) {
        writer.push_back_field(tag::SenderCompID, std::string_view("GATEWAY"));
    }
    if (skip != tag::TargetCompID) {
        writer.push_back_field(tag::TargetCompID, std::string_view("CLIENT"));
    }
    if (skip != tag::MsgSeqNum) {
        writer.push_back_field(tag::MsgSeqNum, 1);
    }
    if (skip != tag::SendingTime) {
        writer.push_back_field(tag::SendingTime, std::string_view("20260721-10:00:00"));
    }
    if (skip != tag::ClOrdID) {
        writer.push_back_field(tag::ClOrdID, std::string_view("ORDER-1"));
    }
    if (skip != tag::Side) {
        writer.push_back_field(tag::Side, '1'); // Buy -- a defined Side value
    }
    if (skip != tag::OrdType) {
        writer.push_back_field(tag::OrdType, '2'); // Limit -- a defined OrdType value
    }
    if (skip != tag::TransactTime) {
        writer.push_back_field(tag::TransactTime, std::string_view("20260721-10:00:00"));
    }
}

// A fully-formed NewOrderSingle passes validation with no reject.
TEST(FixMessageValidatorTest, ConformingNewOrderSinglePasses) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer);
    const std::string_view wire = writer.finish();
    ASSERT_FALSE(wire.empty());

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_TRUE(reject.ok()) << "unexpected reject reason " << static_cast<int>(reject.reason) << " on tag " << reject.ref_tag;
}

// ----- Missing required tag -----------------------------------------------------

// Removing a required tag is reported as RequiredTagMissing naming that exact tag.
TEST(FixMessageValidatorTest, MissingRequiredClOrdIDNamesTag11) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::ClOrdID); // omit ClOrdID (11)
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()); // frames cleanly...
    const FixReject reject = FixMessageValidator(reader).validate();

    EXPECT_EQ(reject.reason, RejectReason::RequiredTagMissing); // ...but fails validation
    EXPECT_EQ(reject.ref_tag, tag::ClOrdID);
    EXPECT_EQ(reject.ref_msg_type, "D");
}

// A different missing required tag is named correctly too -- the reject is not a
// generic "something missing" but points at the specific absent field.
TEST(FixMessageValidatorTest, MissingRequiredOrdTypeNamesTag40) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::OrdType); // omit OrdType (40)
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::RequiredTagMissing);
    EXPECT_EQ(reject.ref_tag, tag::OrdType);
}

// The reject's text names the tag both by number and by dictionary name, so a log
// line or FIX Reject Text says which tag, not just an opaque code.
TEST(FixMessageValidatorTest, DescribeNamesTheMissingTag) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::ClOrdID);
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    const FixReject reject = FixMessageValidator(reader).validate();
    char text[128];
    const std::string_view described = reject.describe(text, sizeof(text));
    EXPECT_NE(described.find("RequiredTagMissing"), std::string_view::npos);
    EXPECT_NE(described.find("ClOrdID"), std::string_view::npos);
    EXPECT_NE(described.find("11"), std::string_view::npos);
}

// ----- Duplicate tag ------------------------------------------------------------

// A repeated tag is reported as TagAppearsMoreThanOnce naming the duplicated tag.
TEST(FixMessageValidatorTest, DuplicateClOrdIDNamesTag11) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer);
    writer.push_back_field(tag::ClOrdID, std::string_view("ORDER-2")); // a second ClOrdID
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::TagAppearsMoreThanOnce);
    EXPECT_EQ(reject.ref_tag, tag::ClOrdID);
}

// ----- Invalid (undefined) tag --------------------------------------------------

// A tag the dictionary does not define is reported as InvalidTagNumber naming it.
TEST(FixMessageValidatorTest, UndefinedTagIsInvalidTagNumber) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer);
    writer.push_back_field(9999, std::string_view("x")); // 9999 is not a defined FIX tag
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::InvalidTagNumber);
    EXPECT_EQ(reject.ref_tag, 9999);
}

// A tag that is defined in the dictionary but not part of this message type is
// reported as TagNotDefinedForThisMessage. HeartBtInt (108) is a Logon field.
TEST(FixMessageValidatorTest, TagNotForThisMessageIsRejected) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer);
    writer.push_back_field(tag::HeartBtInt, 30); // defined (108), but not for NewOrderSingle
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid());
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::TagNotDefinedForThisMessage);
    EXPECT_EQ(reject.ref_tag, tag::HeartBtInt);
}

// ----- Wrong data type ----------------------------------------------------------

// A non-integer value in an integer field (MsgSeqNum is SEQNUM) is IncorrectDataFormat.
TEST(FixMessageValidatorTest, NonIntegerSeqNumIsIncorrectDataFormat) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::MsgSeqNum);
    writer.push_back_field(tag::MsgSeqNum, std::string_view("12abc")); // not an integer
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()); // framing is fine -- this is the point I got wrong before
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::IncorrectDataFormat);
    EXPECT_EQ(reject.ref_tag, tag::MsgSeqNum);
    EXPECT_EQ(reject.value, "12abc");
}

// A non-decimal value in a quantity field (OrderQty is QTY) is IncorrectDataFormat.
TEST(FixMessageValidatorTest, NonDecimalOrderQtyIsIncorrectDataFormat) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer);
    writer.push_back_field(tag::OrderQty, std::string_view("12abc")); // not a decimal
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::IncorrectDataFormat);
    EXPECT_EQ(reject.ref_tag, tag::OrderQty);
}

// An out-of-range timestamp (valid shape, impossible hour) is IncorrectDataFormat.
TEST(FixMessageValidatorTest, BadTimestampIsIncorrectDataFormat) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::TransactTime);
    writer.push_back_field(tag::TransactTime, std::string_view("20260721-99:00:00")); // hour 99
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::IncorrectDataFormat);
    EXPECT_EQ(reject.ref_tag, tag::TransactTime);
}

// A value not defined for an enumerated field (Side has no 'Z') is ValueIsIncorrect.
TEST(FixMessageValidatorTest, UndefinedSideValueIsValueIsIncorrect) {
    char buffer[256];
    FixMessageWriter writer(buffer, sizeof(buffer));
    push_mandatory(writer, tag::Side);
    writer.push_back_field(tag::Side, 'Z'); // not one of the defined Side values
    const std::string_view wire = writer.finish();

    FixMessageReader reader(wire);
    const FixReject reject = FixMessageValidator(reader).validate();
    EXPECT_EQ(reject.reason, RejectReason::ValueIsIncorrect);
    EXPECT_EQ(reject.ref_tag, tag::Side);
    EXPECT_EQ(reject.value, "Z");
}

} // namespaces
