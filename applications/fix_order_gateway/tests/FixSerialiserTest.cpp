// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixSerialiser.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

// Regression test for FixSerialiser's application-tag whitelist. The whitelist
// once omitted the FIX Reject (35=3) reference fields, so a Reject serialised with
// only Text (58) and the peer rejected it in turn. This test builds a 35=3 and
// asserts every reference field survives serialisation.

using fix_order_gateway::FixMessage;
using fix_order_gateway::FixSerialiser;
namespace tag = fix_codec::tag;

namespace {

TEST(FixSerialiserTest, SerialisesAllRejectReferenceFields) {
    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    const FixSerialiser serialiser("GATEWAY", "CLIENT", clock);

    FixMessage reject{};
    reject.set(tag::MsgType, fix_codec::msg_type::Reject);
    reject.set(tag::RefSeqNum, 7);
    reject.set(tag::RefTagID, 40);
    reject.set(tag::RefMsgType, std::string_view("D"));
    reject.set(tag::SessionRejectReason, 1);
    reject.set(tag::Text, std::string_view("RequiredTagMissing: tag 40 (OrdType) in D"));

    const std::string wire = serialiser.serialise(reject, 3);

    // A well-framed message: BodyLength and Checksum account for the new fields.
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.msg_type(), "3");

    // Every reference field is present (each of these was dropped before the fix).
    EXPECT_EQ(reader.find(tag::RefSeqNum).as_int(), 7);
    EXPECT_EQ(reader.find(tag::RefTagID).as_int(), 40);
    EXPECT_EQ(reader.find(tag::RefMsgType).as_string_view(), "D");
    EXPECT_EQ(reader.find(tag::SessionRejectReason).as_int(), 1);
    EXPECT_EQ(reader.find(tag::Text).as_string_view(), "RequiredTagMissing: tag 40 (OrdType) in D");
}

} // namespaces
