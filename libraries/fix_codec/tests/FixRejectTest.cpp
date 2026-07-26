// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_codec/FixReject.hpp>

#include <array>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/fix_dictionary.hpp>

/*
These reject reasons leave the venue: the gateway turns a FixReject into a FIX
Reject (35=3), where reason_text() supplies part of tag 58 and the numeric enum
value is tag 373. So a reason with no switch arm does not merely read oddly in a
log -- it sends a client "Unknown" for a fault the venue diagnosed precisely.

What these tests do *not* need to guard is a missing switch arm: -Werror=switch
already fails the build for that, verified by deleting one. What the compiler
cannot see is a copy-paste slip that gives two reasons the same text, or a wire
value that drifts from the FIX-defined code -- both verified as caught here -- and
the buffer behaviour of describe(), which was the bulk of this header's uncovered
lines.
*/

namespace {

constexpr std::array<fix_codec::RejectReason, 8> all_reasons{
    fix_codec::RejectReason::None,
    fix_codec::RejectReason::InvalidTagNumber,
    fix_codec::RejectReason::RequiredTagMissing,
    fix_codec::RejectReason::TagNotDefinedForThisMessage,
    fix_codec::RejectReason::ValueIsIncorrect,
    fix_codec::RejectReason::IncorrectDataFormat,
    fix_codec::RejectReason::TagAppearsMoreThanOnce,
    fix_codec::RejectReason::IncorrectNumInGroupCount,
};

TEST(FixRejectTest, EveryReasonHasItsOwnText) {
    std::set<std::string_view> seen;
    for (const fix_codec::RejectReason reason : all_reasons) {
        const std::string_view text = fix_codec::reason_text(reason);
        EXPECT_FALSE(text.empty()) << "reason " << static_cast<int>(reason) << " has no text";
        EXPECT_NE(text, "Unknown") << "reason " << static_cast<int>(reason) << " fell through the switch";
        EXPECT_TRUE(seen.insert(text).second) << "text '" << text << "' is used by more than one reason";
    }
    EXPECT_EQ(seen.size(), all_reasons.size());
}

// An enum value outside the listed set is the only case that should read
// "Unknown"; this also fixes the fall-through as deliberate rather than accidental.
TEST(FixRejectTest, AnUnlistedReasonReadsAsUnknown) {
    EXPECT_EQ(fix_codec::reason_text(static_cast<fix_codec::RejectReason>(999)), "Unknown");
}

// The numeric values are FIX SessionRejectReason (tag 373) codes and go on the
// wire, so they are part of the protocol rather than an implementation detail.
TEST(FixRejectTest, ReasonValuesAreTheFixSessionRejectReasonCodes) {
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::InvalidTagNumber), 0);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::RequiredTagMissing), 1);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::TagNotDefinedForThisMessage), 2);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::ValueIsIncorrect), 5);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::IncorrectDataFormat), 6);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::TagAppearsMoreThanOnce), 13);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::IncorrectNumInGroupCount), 16);
    EXPECT_EQ(static_cast<int>(fix_codec::RejectReason::None), -1);
}

TEST(FixRejectTest, DefaultConstructedRejectIsOk) {
    const fix_codec::FixReject reject;
    EXPECT_TRUE(reject.ok());
    EXPECT_EQ(reject.reason, fix_codec::RejectReason::None);
    EXPECT_EQ(reject.ref_tag, 0);
}

TEST(FixRejectTest, AnyNonNoneReasonIsNotOk) {
    for (const fix_codec::RejectReason reason : all_reasons) {
        const fix_codec::FixReject reject{reason, 11, "D", {}};
        EXPECT_EQ(reject.ok(), reason == fix_codec::RejectReason::None);
    }
}

TEST(FixRejectTest, DescribeNamesTheReasonTheTagAndTheMessageType) {
    std::array<char, 128> buffer{};
    const fix_codec::FixReject reject{fix_codec::RejectReason::RequiredTagMissing, fix_codec::tag::ClOrdID, "D", {}};
    const std::string_view description = reject.describe(buffer.data(), buffer.size());

    EXPECT_NE(description.find("RequiredTagMissing"), std::string_view::npos);
    EXPECT_NE(description.find("11"), std::string_view::npos);
    EXPECT_NE(description.find("ClOrdID"), std::string_view::npos);
    EXPECT_NE(description.find("D"), std::string_view::npos);
    // No value was supplied, so the value clause must be absent entirely.
    EXPECT_EQ(description.find("value"), std::string_view::npos);
}

TEST(FixRejectTest, DescribeIncludesTheOffendingValueWhenThereIsOne) {
    std::array<char, 128> buffer{};
    const fix_codec::FixReject reject{fix_codec::RejectReason::IncorrectDataFormat, fix_codec::tag::OrderQty, "D", "12abc"};
    const std::string_view description = reject.describe(buffer.data(), buffer.size());

    EXPECT_NE(description.find("IncorrectDataFormat"), std::string_view::npos);
    EXPECT_NE(description.find("OrderQty"), std::string_view::npos);
    EXPECT_NE(description.find("12abc"), std::string_view::npos);
    EXPECT_NE(description.find("value"), std::string_view::npos);
}

/*
describe() writes into a caller buffer and must never run past the capacity it was
given. The buffer below is larger than the capacity passed in, so the tail acts as a
guard region: snprintf may legitimately place its NUL terminator in the final byte
of the declared capacity, but nothing beyond it may be touched.
*/
TEST(FixRejectTest, DescribeTruncatesRatherThanOverrunningTheGivenCapacity) {
    constexpr size_t capacity = 16;
    std::array<char, 64> backing{};
    backing.fill('\xEE');
    const fix_codec::FixReject reject{fix_codec::RejectReason::TagNotDefinedForThisMessage, fix_codec::tag::ClOrdID, "NewOrderSingle",
                                      "a-long-offending-value"};
    const std::string_view description = reject.describe(backing.data(), capacity);

    EXPECT_LT(description.size(), capacity) << "the returned view must exclude the NUL terminator";
    for (size_t index = capacity; index < backing.size(); ++index) {
        EXPECT_EQ(backing[index], '\xEE') << "describe wrote past the capacity it was given, at offset " << index;
    }
}

TEST(FixRejectTest, DescribeRejectsANullOrZeroLengthBuffer) {
    std::array<char, 32> buffer{};
    const fix_codec::FixReject reject{fix_codec::RejectReason::ValueIsIncorrect, fix_codec::tag::Side, "D", "Z"};

    EXPECT_TRUE(reject.describe(nullptr, buffer.size()).empty());
    EXPECT_TRUE(reject.describe(buffer.data(), 0).empty());
}

// Every reason must render without crashing and produce something non-empty,
// including None, which the gateway can legitimately try to describe.
TEST(FixRejectTest, DescribeProducesTextForEveryReason) {
    for (const fix_codec::RejectReason reason : all_reasons) {
        std::array<char, 128> buffer{};
        const fix_codec::FixReject reject{reason, fix_codec::tag::ClOrdID, "D", {}};
        const std::string_view description = reject.describe(buffer.data(), buffer.size());
        EXPECT_FALSE(description.empty()) << "reason " << static_cast<int>(reason) << " described as nothing";
        EXPECT_NE(description.find(fix_codec::reason_text(reason)), std::string_view::npos);
    }
}

} // namespaces
