// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_codec/FixChecksum.hpp>

#include <string>
#include <string_view>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

// A checksum is the sum of every byte modulo 256.
TEST(FixChecksumTest, ComputeSumsBytesModulo256) {
    EXPECT_EQ(fix_codec::compute_checksum(""), 0U);
    EXPECT_EQ(fix_codec::compute_checksum("\x01\x02\x03"), 6U);
    // 0xFF + 0x02 == 0x101 == 257, mod 256 == 1.
    EXPECT_EQ(fix_codec::compute_checksum("\xff\x02"), 1U);
}

TEST(FixChecksumTest, MatchesWellFormedChecksum) {
    const std::string body = "8=FIXT.1.1\x01"
                             "9=5\x01"
                             "35=0\x01";
    const unsigned int expected = fix_codec::compute_checksum(body);
    const std::string digits = fmt::format("{:03}", expected);
    EXPECT_TRUE(fix_codec::checksum_matches(body, digits));
}

TEST(FixChecksumTest, RejectsWrongChecksum) {
    const std::string body = "8=FIXT.1.1\x01"
                             "9=5\x01"
                             "35=0\x01";
    EXPECT_FALSE(fix_codec::checksum_matches(body, "000"));
}

TEST(FixChecksumTest, RejectsWrongLengthOrNonDigits) {
    EXPECT_FALSE(fix_codec::checksum_matches("abc", "12"));   // too short
    EXPECT_FALSE(fix_codec::checksum_matches("abc", "1234")); // too long
    EXPECT_FALSE(fix_codec::checksum_matches("abc", "12x"));  // non-digit
    EXPECT_FALSE(fix_codec::checksum_matches("abc", "+12"));  // sign is not a digit
}

} // namespaces
