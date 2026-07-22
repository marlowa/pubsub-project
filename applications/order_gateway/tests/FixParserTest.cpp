// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixParser.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/FixReject.hpp>
#include <fix_codec/fix_dictionary.hpp>
#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

// Parity tests for FixParser after its reimplementation as a stream driver over
// fix_codec::FixMessageReader. They pin the stream contract the gateway depends on:
// the consumed-byte count returned by feed(), which complete messages are
// dispatched, and how incomplete / malformed / bad-checksum input is handled.

using order_gateway::FixParser;
using order_gateway::ParsedFixMessage;
namespace tag = fix_codec::tag;

namespace {

// One dispatched message, copied out of the transient (non-copyable, view-backed)
// ParsedFixMessage before the callback returns.
struct Captured {
    std::string msg_type;
    std::string cl_ord_id;
    int field_count{0};
};

// Builds a complete, well-framed, dictionary-valid NewOrderSingle -- correct
// BodyLength and Checksum, and every tag NewOrderSingle requires (MsgType,
// SenderCompID, TargetCompID, MsgSeqNum, SendingTime, ClOrdID, Side, OrdType,
// TransactTime) so it passes FixMessageValidator. When @p include_ord_type is
// false the (required) OrdType is omitted, producing a well-framed message that
// fails validation with RequiredTagMissing.
std::string build_message(std::string_view cl_ord_id, bool include_ord_type = true) {
    char buffer[256];
    fix_codec::FixMessageWriter writer(buffer, sizeof(buffer));
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::NewOrderSingle);
    writer.push_back_field(tag::SenderCompID, std::string_view("CLIENT"));
    writer.push_back_field(tag::TargetCompID, std::string_view("GATEWAY"));
    writer.push_back_field(tag::MsgSeqNum, 1);
    writer.push_back_field(tag::SendingTime, std::string_view("20260722-10:00:00"));
    writer.push_back_field(tag::ClOrdID, cl_ord_id);
    writer.push_back_field(tag::Symbol, std::string_view("AAPL"));
    writer.push_back_field(tag::Side, '1');
    if (include_ord_type) {
        writer.push_back_field(tag::OrdType, '2');
    }
    writer.push_back_field(tag::TransactTime, std::string_view("20260722-10:00:00"));
    return std::string(writer.finish());
}

size_t feed(FixParser& parser, const std::string& bytes) {
    return parser.feed(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// One message that framed cleanly but failed dictionary validation.
struct Rejected {
    int reason{0};
    int ref_tag{0};
};

class FixParserTest : public ::testing::Test {
  protected:
    pubsub_itc_fw::LoggerWithSink logger_;
    std::vector<Captured> captured_;
    std::vector<Rejected> rejected_;

    FixParser make_parser() {
        return FixParser(
            logger_.logger,
            [this](const ParsedFixMessage& message) {
                captured_.push_back(Captured{std::string(message.msg_type()), std::string(message.get(tag::ClOrdID)), message.size()});
            },
            [this](const ParsedFixMessage&, const fix_codec::FixReject& reject) {
                rejected_.push_back(Rejected{static_cast<int>(reject.reason), reject.ref_tag});
            });
    }
};

TEST_F(FixParserTest, DispatchesOneCompleteMessageAndConsumesItWholly) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1");

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    ASSERT_EQ(captured_.size(), 1U);
    EXPECT_EQ(captured_[0].msg_type, "D");
    EXPECT_EQ(captured_[0].cl_ord_id, "ORDER-1");
}

TEST_F(FixParserTest, DispatchesTwoBackToBackMessagesInOneWindow) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1") + build_message("ORDER-2");

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    ASSERT_EQ(captured_.size(), 2U);
    EXPECT_EQ(captured_[0].cl_ord_id, "ORDER-1");
    EXPECT_EQ(captured_[1].cl_ord_id, "ORDER-2");
}

TEST_F(FixParserTest, LeavesAnIncompleteTrailingMessageUnconsumed) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1");
    // Drop the last few bytes so the Checksum field is not fully present.
    const std::string truncated = wire.substr(0, wire.size() - 3);

    const size_t consumed = feed(parser, truncated);

    EXPECT_EQ(consumed, 0U);
    EXPECT_TRUE(captured_.empty());
}

TEST_F(FixParserTest, CompletesAMessageDeliveredAcrossTwoFeeds) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1");

    // The MirroredBuffer re-presents the same bytes plus new ones; feed is stateless.
    EXPECT_EQ(feed(parser, wire.substr(0, wire.size() - 3)), 0U);
    EXPECT_TRUE(captured_.empty());
    const size_t consumed = feed(parser, wire);
    EXPECT_EQ(consumed, wire.size());
    ASSERT_EQ(captured_.size(), 1U);
    EXPECT_EQ(captured_[0].cl_ord_id, "ORDER-1");
}

TEST_F(FixParserTest, ConsumesButDoesNotDispatchABadChecksumMessage) {
    FixParser parser = make_parser();
    std::string wire = build_message("ORDER-1");
    // Flip a byte inside a field value: framing (BodyLength, "10=" position) stays
    // intact so the message frames, but the stored checksum no longer matches.
    const size_t sender = wire.find("CLIENT");
    ASSERT_NE(sender, std::string::npos);
    wire[sender] ^= 0x20;

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size()); // whole message skipped
    EXPECT_TRUE(captured_.empty());   // not dispatched
}

TEST_F(FixParserTest, ResyncsPastAMalformedStartToTheNextMessage) {
    FixParser parser = make_parser();
    // A false "8=" start whose second field is not BodyLength, immediately followed
    // by a genuine message. The driver must resync and dispatch the good one.
    std::string junk = "8=X";
    junk.push_back('\x01');
    const std::string good = build_message("ORDER-2");
    const std::string wire = junk + good;

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    ASSERT_EQ(captured_.size(), 1U);
    EXPECT_EQ(captured_[0].cl_ord_id, "ORDER-2");
}

TEST_F(FixParserTest, SkipsLeadingGarbageBeforeAValidMessage) {
    FixParser parser = make_parser();
    const std::string wire = "garbage-with-no-tag" + build_message("ORDER-3");

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    ASSERT_EQ(captured_.size(), 1U);
    EXPECT_EQ(captured_[0].cl_ord_id, "ORDER-3");
}

// A conforming message is dispatched and never reported as a reject.
TEST_F(FixParserTest, DispatchesAValidMessageWithoutRejecting) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1");

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    EXPECT_EQ(captured_.size(), 1U);
    EXPECT_TRUE(rejected_.empty());
}

// A message that frames cleanly (correct BodyLength and Checksum) but omits a
// required tag is delivered to the reject path, not dispatched. It is consumed
// wholly, and the reject names the specific missing tag.
TEST_F(FixParserTest, RejectsAWellFramedMessageThatFailsValidation) {
    FixParser parser = make_parser();
    const std::string wire = build_message("ORDER-1", /*include_ord_type=*/false);

    const size_t consumed = feed(parser, wire);

    EXPECT_EQ(consumed, wire.size());
    EXPECT_TRUE(captured_.empty());
    ASSERT_EQ(rejected_.size(), 1U);
    EXPECT_EQ(rejected_[0].reason, static_cast<int>(fix_codec::RejectReason::RequiredTagMissing));
    EXPECT_EQ(rejected_[0].ref_tag, tag::OrdType);
}

} // namespaces
