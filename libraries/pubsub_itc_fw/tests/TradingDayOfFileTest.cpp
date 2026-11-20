// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for the trading day each small file records, and for the stores that write it: a file left from another trading
// day must be told from one of the present day, so that a component never takes it for a restart (BUG-0137).

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ComponentArguments.hpp>
#include <pubsub_itc_fw/ComponentStatementStore.hpp>
#include <pubsub_itc_fw/EpochStore.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/TradingDayOfFile.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

constexpr int32_t today = 20'261'116;
constexpr int32_t yesterday = 20'261'113;

class TradingDayOfFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("trading_day_of_file_test");
        path_ = dir_ + "/state";
    }

    void TearDown() override {
        ::unlink(path_.c_str());
        ::unlink((path_ + ".tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    void write_raw(const std::string& contents) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    [[nodiscard]] std::string read_raw() const {
        std::ifstream in(path_, std::ios::binary);
        std::stringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }

    std::string dir_;
    std::string path_;
};

} // namespaces

TEST_F(TradingDayOfFileTest, TheFieldIsReadAndRemoved) {
    std::string_view text = "trading_day=20261116 17\n";
    EXPECT_EQ(pubsub_itc_fw::take_trading_day(text), today);
    EXPECT_EQ(text, "17\n");
}

TEST_F(TradingDayOfFileTest, TextWithoutTheFieldIsLeftAsItIs) {
    std::string_view text = "17\n";
    EXPECT_EQ(pubsub_itc_fw::take_trading_day(text), 0);
    EXPECT_EQ(text, "17\n");
    std::string_view short_day = "trading_day=2026 17\n";
    EXPECT_EQ(pubsub_itc_fw::take_trading_day(short_day), 0) << "a day of fewer than eight digits is not a day";
}

TEST_F(TradingDayOfFileTest, AnAbsentFileRecordsNothingAndOneWithoutTheFieldRecordsZero) {
    EXPECT_FALSE(pubsub_itc_fw::recorded_trading_day(path_).has_value());
    write_raw("17\n");
    EXPECT_EQ(pubsub_itc_fw::recorded_trading_day(path_), std::optional<int32_t>{0});
    write_raw("trading_day=20261113 17\n");
    EXPECT_EQ(pubsub_itc_fw::recorded_trading_day(path_), std::optional<int32_t>{yesterday});
}

TEST_F(TradingDayOfFileTest, OnlyAFileOfAnotherDayOrOfNoDayIsNamed) {
    EXPECT_EQ(pubsub_itc_fw::another_days_file("/x", std::nullopt, today), "");
    EXPECT_EQ(pubsub_itc_fw::another_days_file("/x", today, today), "");
    EXPECT_EQ(pubsub_itc_fw::another_days_file("/x", yesterday, today), "/x belongs to trading day 20261113, not 20261116");
    EXPECT_EQ(pubsub_itc_fw::another_days_file("/x", 0, today), "/x records no trading day, so it cannot be known to belong to trading day 20261116");
    EXPECT_EQ(pubsub_itc_fw::files_of_another_day(today, {{"/a", yesterday}, {"/b", today}, {"/c", std::nullopt}, {"/d", 0}}),
              "/a belongs to trading day 20261113, not 20261116; /d records no trading day, so it cannot be known to belong to trading day 20261116");
}

TEST_F(TradingDayOfFileTest, TheEpochIsWrittenWithItsDayAndReadBack) {
    const pubsub_itc_fw::EpochStore store(path_, today);
    ASSERT_TRUE(store.store(7));
    EXPECT_EQ(read_raw(), "trading_day=20261116 7\n");
    EXPECT_EQ(store.load(), 7);
    EXPECT_EQ(store.recorded_trading_day(), std::optional<int32_t>{today});
}

TEST_F(TradingDayOfFileTest, AnEpochWrittenWithoutADayIsStillRead) {
    write_raw("9\n");
    const pubsub_itc_fw::EpochStore store(path_, today);
    EXPECT_EQ(store.load(), 9);
    EXPECT_EQ(store.recorded_trading_day(), std::optional<int32_t>{0});
}

TEST_F(TradingDayOfFileTest, AStoreGivenNoDayWritesNone) {
    const pubsub_itc_fw::EpochStore store(path_, 0);
    ASSERT_TRUE(store.store(7));
    EXPECT_EQ(read_raw(), "7\n");
}

TEST_F(TradingDayOfFileTest, TheLeasePromiseIsWrittenWithItsDayAndReadBack) {
    pubsub_itc_fw::LeasePromiseStore store(path_, "boot-a", today);
    const auto until = std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{5};
    ASSERT_TRUE(store.record(pubsub_itc_fw::LeasePromiseRecord{2, until, {}}));
    EXPECT_EQ(read_raw().rfind("trading_day=20261116 boot-a 2 ", 0), 0U) << read_raw();
    ASSERT_TRUE(store.load().has_value());
    EXPECT_EQ(store.load()->promised_to, 2);
    EXPECT_EQ(store.recorded_trading_day(), std::optional<int32_t>{today});
}

TEST_F(TradingDayOfFileTest, TheStatementsAreWrittenAfterTheirDayAndReadBack) {
    const pubsub_itc_fw::ComponentStatementStore store(path_, today);
    pubsub_itc_fw::ComponentStatementStore::Statements statements;
    statements[3] = pubsub_itc_fw::LeaderStatement{1, 4, 9, false};
    ASSERT_TRUE(store.save(statements));
    EXPECT_EQ(read_raw(), "trading_day=20261116\n3 1 4 9 0\n");
    EXPECT_EQ(store.load().size(), 1U);
    EXPECT_EQ(store.recorded_trading_day(), std::optional<int32_t>{today});
}

TEST_F(TradingDayOfFileTest, AComponentIsGivenItsTradingDayOnItsCommandLine) {
    const char* const good[] = {"arbiter", "arbiter.log", "arbiter.toml", "--trading-day", "20261116"};
    std::ostringstream errors;
    const std::optional<pubsub_itc_fw::ComponentArguments> arguments = pubsub_itc_fw::parse_component_arguments(5, good, "arbiter", errors);
    ASSERT_TRUE(arguments.has_value());
    EXPECT_EQ(arguments->trading_day, today);
    EXPECT_EQ(arguments->config_file, "arbiter.toml");
    const char* const missing[] = {"arbiter", "arbiter.log", "arbiter.toml"};
    EXPECT_FALSE(pubsub_itc_fw::parse_component_arguments(3, missing, "arbiter", errors).has_value());
    const char* const not_a_day[] = {"arbiter", "arbiter.log", "arbiter.toml", "--trading-day", "20261131"};
    EXPECT_FALSE(pubsub_itc_fw::parse_component_arguments(5, not_a_day, "arbiter", errors).has_value());
    EXPECT_NE(errors.str().find("is not a date written as YYYYMMDD"), std::string::npos);
}
