// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCommandSearch.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include "TestCommandIdentity.hpp"

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t segment_size = 4096;
constexpr int64_t one_second_ns = 1'000'000'000LL;
constexpr int64_t the_whole_wal = std::numeric_limits<int64_t>::min();

class WalCommandSearchTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = tests_common::make_scratch_directory("wal_command_search_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The WAL's helper thread creates the next segment ahead of the writer, so the WAL is shut down, and
        // the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    // Appends a command's record at the given time.
    void append_command(const std::string& identity, int64_t wall_time_ns, int16_t pdu_id = test_command_pdu_id) {
        const std::vector<uint8_t> bytes = test_command_bytes(identity);
        ++last_seq_no_;
        wal_->append(last_seq_no_, pdu_id, bytes.data(), static_cast<int>(bytes.size()), wall_time_ns, 0);
    }

    // One command a second, numbered from 1, so that the WAL spans many segments.
    void append_commands(int count) {
        for (int number = 1; number <= count; ++number) {
            append_command("order-" + std::to_string(number), number * one_second_ns);
        }
    }

    std::string dir_;
    std::optional<Wal> wal_;
    int64_t last_seq_no_{0};
    TestCommandIdentity identity_;
};

} // namespaces

TEST_F(WalCommandSearchTest, ARecentCommandIsFoundAndOneNeverWrittenIsNot) {
    append_commands(500);
    WalCommandSearch search(dir_, identity_);
    EXPECT_TRUE(search.holds("order-500", 499 * one_second_ns));
    EXPECT_FALSE(search.holds("order-501", 499 * one_second_ns));
}

TEST_F(WalCommandSearchTest, TheIdentityMustMatchExactly) {
    append_commands(10);
    WalCommandSearch search(dir_, identity_);
    EXPECT_FALSE(search.holds("order-5x", the_whole_wal));
    EXPECT_FALSE(search.holds("order-", the_whole_wal));
    EXPECT_TRUE(search.holds("order-5", the_whole_wal));
}

// Only as far back as the command's own time, less the margin for the clocks, is read. A command older than
// that is not looked for, because a command sent again cannot have been written before it was first sent.
TEST_F(WalCommandSearchTest, OnlyAsMuchOfTheWalIsReadAsTheCommandsTimeRequires) {
    append_commands(500);
    WalCommandSearch search(dir_, identity_);
    EXPECT_TRUE(search.holds("order-499", 499 * one_second_ns));
    EXPECT_LT(search.records_read(), 500);
    EXPECT_FALSE(search.holds("order-1", 499 * one_second_ns)) << "order-1 is older than the time given";
    EXPECT_TRUE(search.holds("order-1", 1 * one_second_ns));
    EXPECT_EQ(search.records_read(), 500) << "every record read once, and none twice";
}

TEST_F(WalCommandSearchTest, ACommandWrittenAfterTheSearchWasCreatedIsFound) {
    append_commands(100);
    WalCommandSearch search(dir_, identity_);
    EXPECT_FALSE(search.holds("later", 100 * one_second_ns));
    append_command("later", 101 * one_second_ns);
    EXPECT_TRUE(search.holds("later", 100 * one_second_ns));
}

TEST_F(WalCommandSearchTest, ARecordThatIsNotACommandIsNotFound) {
    append_command("order-1", one_second_ns, test_other_pdu_id);
    WalCommandSearch search(dir_, identity_);
    EXPECT_FALSE(search.holds("order-1", the_whole_wal));
}

TEST_F(WalCommandSearchTest, AnEmptyWalHoldsNothing) {
    WalCommandSearch search(dir_, identity_);
    EXPECT_FALSE(search.holds("order-1", the_whole_wal));
    append_command("order-1", one_second_ns);
    EXPECT_TRUE(search.holds("order-1", the_whole_wal));
}

} // namespaces
