// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fix_codec/FixField.hpp>

#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using fix_codec::FixField;

FixField make(int tag, std::string_view value) {
    return FixField{tag, value};
}

TEST(FixFieldTest, StringAndEmpty) {
    EXPECT_TRUE(make(58, "").empty());
    EXPECT_FALSE(make(58, "hello").empty());
    EXPECT_EQ(make(58, "hello").as_string_view(), "hello");
}

TEST(FixFieldTest, IntegerAccessors) {
    EXPECT_EQ(make(38, "100").as_int(), 100);
    EXPECT_EQ(make(38, "-7").as_int(), -7);
    EXPECT_EQ(make(34, "9000000000").as_int64(), 9000000000LL);
    EXPECT_EQ(make(38, "4294967000").as_uint(), 4294967000U);
    // Malformed or trailing junk returns the fallback, not a partial parse.
    EXPECT_EQ(make(38, "12x").as_int(-1), -1);
    EXPECT_EQ(make(38, "").as_int(42), 42);
}

TEST(FixFieldTest, CharAndBool) {
    EXPECT_EQ(make(54, "1").as_char(), '1');
    EXPECT_EQ(make(54, "").as_char('?'), '?');
    EXPECT_EQ(make(54, "12").as_char('?'), '?'); // more than one char -> fallback
    EXPECT_TRUE(make(43, "Y").as_bool());
    EXPECT_FALSE(make(43, "N").as_bool());
}

TEST(FixFieldTest, DecimalMantissaExponent) {
    int64_t mantissa = 0;
    int exponent = 0;

    ASSERT_TRUE(make(44, "123.45").as_decimal(mantissa, exponent));
    EXPECT_EQ(mantissa, 12345);
    EXPECT_EQ(exponent, -2);

    ASSERT_TRUE(make(44, "-0.001").as_decimal(mantissa, exponent));
    EXPECT_EQ(mantissa, -1);
    EXPECT_EQ(exponent, -3);

    ASSERT_TRUE(make(44, "100").as_decimal(mantissa, exponent));
    EXPECT_EQ(mantissa, 100);
    EXPECT_EQ(exponent, 0);

    EXPECT_FALSE(make(44, "").as_decimal(mantissa, exponent));
    EXPECT_FALSE(make(44, "1.2.3").as_decimal(mantissa, exponent));
    EXPECT_FALSE(make(44, "1e5").as_decimal(mantissa, exponent));
}

TEST(FixFieldTest, UtcTimestampToNanos) {
    // 1970-01-01 00:00:00 UTC is the epoch.
    EXPECT_EQ(make(52, "19700101-00:00:00").as_utc_timestamp_ns(), 0);
    // One second past the epoch.
    EXPECT_EQ(make(52, "19700101-00:00:01").as_utc_timestamp_ns(), 1000000000LL);
    // Millisecond fractional part is scaled to nanoseconds.
    EXPECT_EQ(make(52, "19700101-00:00:00.123").as_utc_timestamp_ns(), 123000000LL);
    // Full nanosecond precision is preserved.
    EXPECT_EQ(make(52, "19700101-00:00:00.123456789").as_utc_timestamp_ns(), 123456789LL);
    // Malformed input returns the fallback.
    EXPECT_EQ(make(52, "not-a-time").as_utc_timestamp_ns(-1), -1);
    EXPECT_EQ(make(52, "").as_utc_timestamp_ns(), 0);
}

} // namespaces
