// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cctype>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>

using namespace pubsub_itc_fw;

TEST(FwLogLevelTest, AsStringForEachLevel) {
    EXPECT_EQ(FwLogLevel{FwLogLevel::Trace}.as_string(), "TRACE   ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Debug}.as_string(), "DEBUG   ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Info}.as_string(), "INFO    ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Notice}.as_string(), "NOTICE  ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Warning}.as_string(), "WARNING ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Error}.as_string(), "ERROR   ");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Critical}.as_string(), "CRITICAL");
    EXPECT_EQ(FwLogLevel{FwLogLevel::Alert}.as_string(), "ALERT   ");
}

TEST(FwLogLevelTest, AsStringIsAlwaysEightCharacters) {
    EXPECT_EQ(FwLogLevel{FwLogLevel::Info}.as_string().size(), 8u);
    EXPECT_EQ(FwLogLevel{FwLogLevel::Critical}.as_string().size(), 8u);
}

TEST(FwLogLevelTest, AsStringReturnsUnknownForOutOfRangeTag) {
    const FwLogLevel level{static_cast<FwLogLevel::LogLevelTag>(99)};

    EXPECT_EQ(level.as_string(), "UNKNOWN ");
}

TEST(FwLogLevelTest, FromStringAcceptsLowerAndUpperCaseForEveryLevel) {
    const auto check_both_cases = [](const std::string& text, FwLogLevel::LogLevelTag expected) {
        FwLogLevel lower{FwLogLevel::Info};
        EXPECT_TRUE(FwLogLevel::from_string(text, lower));
        EXPECT_EQ(lower, FwLogLevel{expected});

        std::string upper = text;
        for (char& character : upper) {
            character = static_cast<char>(::toupper(static_cast<unsigned char>(character)));
        }
        FwLogLevel upper_level{FwLogLevel::Info};
        EXPECT_TRUE(FwLogLevel::from_string(upper, upper_level));
        EXPECT_EQ(upper_level, FwLogLevel{expected});
    };

    check_both_cases("trace", FwLogLevel::Trace);
    check_both_cases("debug", FwLogLevel::Debug);
    check_both_cases("info", FwLogLevel::Info);
    check_both_cases("notice", FwLogLevel::Notice);
    check_both_cases("warning", FwLogLevel::Warning);
    check_both_cases("error", FwLogLevel::Error);
    check_both_cases("critical", FwLogLevel::Critical);
    check_both_cases("alert", FwLogLevel::Alert);
}

TEST(FwLogLevelTest, FromStringRejectsUnrecognisedInput) {
    FwLogLevel level{FwLogLevel::Info};

    EXPECT_FALSE(FwLogLevel::from_string("verbose", level));
    EXPECT_FALSE(FwLogLevel::from_string("", level));
    EXPECT_FALSE(FwLogLevel::from_string("Info", level)); // mixed case is not accepted
}

TEST(FwLogLevelTest, ComparisonOperatorsFollowSeverityOrder) {
    const FwLogLevel info{FwLogLevel::Info};
    const FwLogLevel error{FwLogLevel::Error};

    EXPECT_TRUE(info < error);
    EXPECT_TRUE(info <= error);
    EXPECT_TRUE(error > info);
    EXPECT_TRUE(error >= info);
    EXPECT_TRUE(info == FwLogLevel{FwLogLevel::Info});
    EXPECT_TRUE(info != error);
    EXPECT_TRUE(info <= FwLogLevel{FwLogLevel::Info});
    EXPECT_TRUE(info >= FwLogLevel{FwLogLevel::Info});
}
