// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "ReportIdentifiers.hpp"
#include "TradingDate.hpp"

namespace matching_engine {
namespace {

ReportIdentifiers::Secret secret_of(uint8_t first) {
    ReportIdentifiers::Secret secret{};
    for (size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<uint8_t>(first + index);
    }
    return secret;
}

constexpr int32_t trading_day = 20261025;

TEST(TradingDateTest, AWallTimeFallsOnItsDateInUtc) {
    // 2026-10-25T00:00:00Z is 1792886400 seconds after the epoch.
    constexpr int64_t midnight = 1'792'886'400LL * 1'000'000'000LL;
    EXPECT_EQ(fix_common::trading_date_of(midnight), 20261025);
    EXPECT_EQ(fix_common::trading_date_of(midnight - 1), 20261024) << "one nanosecond before midnight is the day before";
    EXPECT_EQ(fix_common::trading_date_of(0), 19700101);
    EXPECT_EQ(fix_common::trading_date_of(-1), 19691231);
    // 2024-02-29T12:00:00Z, a leap day: 1709208000 seconds.
    EXPECT_EQ(fix_common::trading_date_of(1'709'208'000LL * 1'000'000'000LL), 20240229);
    EXPECT_EQ(fix_common::trading_date_text(20240229), "2024-02-29");
}

TEST(ReportIdentifiersTest, AnIdentifierIsALetterAndSixteenHexadecimalDigits) {
    ReportIdentifiers identifiers(secret_of(1));
    identifiers.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    const std::string order_id(identifiers.order_id(buffer, 5));
    const std::string exec_id(identifiers.exec_id(buffer, 5, 0));
    ASSERT_EQ(order_id.size(), 17u);
    ASSERT_EQ(exec_id.size(), 17u);
    EXPECT_EQ(order_id[0], 'O');
    EXPECT_EQ(exec_id[0], 'E');
    EXPECT_EQ(order_id.find_first_not_of("0123456789ABCDEF", 1), std::string::npos);
    EXPECT_EQ(exec_id.find_first_not_of("0123456789ABCDEF", 1), std::string::npos);
    EXPECT_NE(order_id.substr(1), exec_id.substr(1)) << "an OrderID and an ExecID made from the same numbers differ, being made with different keys";
}

TEST(ReportIdentifiersTest, TheSameSecretAndDateGiveTheSameIdentifiers) {
    ReportIdentifiers one(secret_of(1));
    ReportIdentifiers other(secret_of(1));
    one.use_trading_date(trading_day);
    other.use_trading_date(trading_day);
    ReportIdentifiers::Buffer first{};
    ReportIdentifiers::Buffer second{};
    EXPECT_EQ(one.exec_id(first, 1234, 7), other.exec_id(second, 1234, 7));
    EXPECT_EQ(one.order_id(first, 1234), other.order_id(second, 1234));
}

TEST(ReportIdentifiersTest, AnotherDateOrAnotherSecretGivesOtherIdentifiers) {
    ReportIdentifiers identifiers(secret_of(1));
    ReportIdentifiers other_secret(secret_of(2));
    identifiers.use_trading_date(trading_day);
    other_secret.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    const std::string today(identifiers.exec_id(buffer, 1, 0));
    const std::string with_other_secret(other_secret.exec_id(buffer, 1, 0));
    identifiers.use_trading_date(trading_day + 1);
    const std::string tomorrow(identifiers.exec_id(buffer, 1, 0));
    EXPECT_NE(today, tomorrow);
    EXPECT_NE(today, with_other_secret);
}

TEST(ReportIdentifiersTest, IdentifiersOfDifferentPositionsDifferAndTurnBackIntoTheirPositions) {
    ReportIdentifiers identifiers(secret_of(1));
    identifiers.use_trading_date(trading_day);
    std::set<std::string> seen;
    ReportIdentifiers::Buffer buffer{};
    const int64_t largest_sequence_number = ReportIdentifiers::sequence_number_limit - 1;
    const int64_t largest_place = ReportIdentifiers::place_limit - 1;
    for (const int64_t sequence_number : {int64_t{0}, int64_t{1}, int64_t{2}, int64_t{1'000'000}, largest_sequence_number}) {
        for (const int64_t place : {int64_t{0}, int64_t{1}, int64_t{99}, largest_place}) {
            const std::string exec_id(identifiers.exec_id(buffer, sequence_number, place));
            EXPECT_TRUE(seen.insert(exec_id).second) << exec_id << " was made twice";
            const auto position = identifiers.position_of(exec_id);
            ASSERT_TRUE(position.has_value());
            EXPECT_EQ(position->kind, ReportIdentifiers::Kind::report);
            EXPECT_EQ(position->sequence_number, sequence_number);
            EXPECT_EQ(position->place, place);
        }
        const std::string order_id(identifiers.order_id(buffer, sequence_number));
        const auto position = identifiers.position_of(order_id);
        ASSERT_TRUE(position.has_value());
        EXPECT_EQ(position->kind, ReportIdentifiers::Kind::order);
        EXPECT_EQ(position->sequence_number, sequence_number);
        EXPECT_EQ(position->place, 0);
    }
}

TEST(ReportIdentifiersTest, ConsecutivePositionsDoNotGiveIdentifiersThatLookConsecutive) {
    ReportIdentifiers identifiers(secret_of(1));
    identifiers.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    const std::string first(identifiers.exec_id(buffer, 100, 0));
    const std::string second(identifiers.exec_id(buffer, 101, 0));
    // Counted digit by digit: two scrambled values agree in about one digit in sixteen, and consecutive numbers written
    // plainly agree in all but the last.
    int same = 0;
    for (size_t index = 1; index < first.size(); ++index) {
        same += first[index] == second[index] ? 1 : 0;
    }
    EXPECT_LT(same, 8) << first << " and " << second;
}

TEST(ReportIdentifiersTest, RefusesANumberOutsideWhatAnIdentifierHolds) {
    ReportIdentifiers identifiers(secret_of(1));
    identifiers.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    EXPECT_THROW(identifiers.exec_id(buffer, ReportIdentifiers::sequence_number_limit, 0), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(identifiers.exec_id(buffer, 1, ReportIdentifiers::place_limit), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(identifiers.exec_id(buffer, -1, 0), pubsub_itc_fw::PreconditionAssertion);
}

TEST(ReportIdentifiersTest, RefusesToMakeAnIdentifierBeforeATradingDateIsGiven) {
    ReportIdentifiers identifiers(secret_of(1));
    ReportIdentifiers::Buffer buffer{};
    EXPECT_THROW(identifiers.exec_id(buffer, 1, 0), pubsub_itc_fw::PreconditionAssertion);
}

TEST(ReportIdentifiersTest, SomethingThatIsNotAnIdentifierHasNoPosition) {
    ReportIdentifiers identifiers(secret_of(1));
    identifiers.use_trading_date(trading_day);
    EXPECT_FALSE(identifiers.position_of("ME-EXEC-1").has_value());
    EXPECT_FALSE(identifiers.position_of("X0123456789ABCDEF").has_value());
    EXPECT_FALSE(identifiers.position_of("E0123456789ABCDEG").has_value());
}

TEST(ReportIdentifiersTest, TheFingerprintTellsSecretsApart) {
    EXPECT_EQ(ReportIdentifiers(secret_of(1)).secret_fingerprint(), ReportIdentifiers(secret_of(1)).secret_fingerprint());
    EXPECT_NE(ReportIdentifiers(secret_of(1)).secret_fingerprint(), ReportIdentifiers(secret_of(2)).secret_fingerprint());
}

class SecretFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::path(::testing::TempDir()) / "report_identifiers_test";
        std::filesystem::create_directories(directory_);
        path_ = (directory_ / "secret.key").string();
    }
    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }
    void write(const std::string& text) {
        std::ofstream(path_) << text;
    }
    std::filesystem::path directory_;
    std::string path_;
};

TEST_F(SecretFileTest, ReadsSixtyFourHexadecimalDigits) {
    write("000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F\n");
    EXPECT_EQ(ReportIdentifiers::read_secret_file(path_), secret_of(0));
}

TEST_F(SecretFileTest, RefusesAFileOfTheWrongLengthOrWithAnotherCharacter) {
    write("0001");
    EXPECT_THROW(ReportIdentifiers::read_secret_file(path_), std::runtime_error);
    write("000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1G");
    EXPECT_THROW(ReportIdentifiers::read_secret_file(path_), std::runtime_error);
}

TEST_F(SecretFileTest, RefusesAFileThatIsNotThere) {
    EXPECT_THROW(ReportIdentifiers::read_secret_file(path_ + ".absent"), std::runtime_error);
}

} // un-named namespace
} // namespaces
