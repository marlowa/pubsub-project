// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/LoggedCommandIdentifiers.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiersBuilder.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include "TestCommandIdentity.hpp"

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t segment_size = 4096;

class LoggedCommandIdentifiersBuilderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = tests_common::make_scratch_directory("logged_command_identifiers_builder_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The WAL's helper thread creates the next segment ahead of the writer, so the WAL is shut down, and
        // the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    void append(const std::string& identity, int16_t pdu_id = test_command_pdu_id) {
        const std::vector<uint8_t> bytes = test_command_bytes(identity);
        ++last_seq_no_;
        wal_->append(last_seq_no_, pdu_id, bytes.data(), static_cast<int>(bytes.size()), last_seq_no_, 0);
    }

    std::vector<LoggedCommandIdentifiersBuilder::Segment> segments() const {
        std::vector<LoggedCommandIdentifiersBuilder::Segment> result;
        for (const uint64_t segment : wal_->segments_on_disk()) {
            result.push_back(LoggedCommandIdentifiersBuilder::Segment{segment, wal_->segment_path(segment)});
        }
        return result;
    }

    static bool wait_until_finished(const LoggedCommandIdentifiersBuilder& builder) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!builder.finished()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return true;
    }

    // The bytes of the record at @p position, as a WalCursor opened there reads them.
    std::string record_at(WalPosition position) const {
        WalCursor cursor;
        cursor.open(dir_, position);
        int64_t record_id = 0;
        const uint8_t* payload = nullptr;
        size_t size = 0;
        if (!cursor.read_next(record_id, payload, size) || cursor.position().segment != position.segment) {
            return {};
        }
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(payload, size);
        return contents.has_value() ? std::string(reinterpret_cast<const char*>(contents->bytes), contents->size) : std::string{};
    }

    std::string dir_;
    std::optional<Wal> wal_;
    int64_t last_seq_no_{0};
    TestCommandIdentity identity_;
};

} // namespaces

TEST_F(LoggedCommandIdentifiersBuilderTest, TheBuiltRecordHoldsEveryCommandInTheWalAcrossSegments) {
    for (int number = 1; number <= 300; ++number) {
        append("order-" + std::to_string(number));
    }
    ASSERT_GT(wal_->segments_on_disk().size(), 3U) << "the WAL should span several segments for this to test anything";

    LoggedCommandIdentifiersBuilder builder;
    builder.start(10000, segments(), identity_);
    ASSERT_TRUE(wait_until_finished(builder));
    LoggedCommandIdentifiers record = builder.take();

    EXPECT_EQ(record.size(), 300U);
    for (int number = 1; number <= 300; ++number) {
        ASSERT_TRUE(record.find(LoggedCommandIdentifiers::identifier("order-" + std::to_string(number))).may_hold) << "order-" << number;
    }
    EXPECT_EQ(builder.records_read(), 300);
}

// The position recorded for each command is where its record starts, in every segment, so that reading there
// finds the command.
TEST_F(LoggedCommandIdentifiersBuilderTest, ThePositionRecordedForEachCommandIsWhereItsRecordIs) {
    for (int number = 1; number <= 300; ++number) {
        append("order-" + std::to_string(number));
    }
    LoggedCommandIdentifiersBuilder builder;
    builder.start(10000, segments(), identity_);
    ASSERT_TRUE(wait_until_finished(builder));
    const LoggedCommandIdentifiers record = builder.take();
    for (int number = 1; number <= 300; ++number) {
        const std::string identity = "order-" + std::to_string(number);
        const LoggedCommandIdentifiers::Lookup found = record.find(LoggedCommandIdentifiers::identifier(identity));
        ASSERT_TRUE(found.position_known) << identity;
        ASSERT_EQ(record_at(found.position), "CMD:" + identity) << identity << " at segment " << found.position.segment << ", byte " << found.position.offset;
    }
}

TEST_F(LoggedCommandIdentifiersBuilderTest, RecordsThatAreNotCommandsAreNotRecorded) {
    append("order-1");
    append("order-2", test_other_pdu_id);
    LoggedCommandIdentifiersBuilder builder;
    builder.start(1000, segments(), identity_);
    ASSERT_TRUE(wait_until_finished(builder));
    const LoggedCommandIdentifiers record = builder.take();
    EXPECT_EQ(record.size(), 1U);
    EXPECT_TRUE(record.find(LoggedCommandIdentifiers::identifier("order-1")).may_hold);
}

TEST_F(LoggedCommandIdentifiersBuilderTest, TheNumberFunctionGivenIsTheOneTheRecordUses) {
    append("order-1");
    LoggedCommandIdentifiersBuilder builder;
    builder.start(1000, segments(), identity_, [](std::string_view /*identity*/) -> uint64_t { return 42; });
    ASSERT_TRUE(wait_until_finished(builder));
    const LoggedCommandIdentifiers record = builder.take();
    EXPECT_TRUE(record.find(42).may_hold);
    EXPECT_EQ(record.number_of("anything"), 42U);
}

TEST_F(LoggedCommandIdentifiersBuilderTest, NothingIsFinishedBeforeTheWalHasBeenRead) {
    const LoggedCommandIdentifiersBuilder builder;
    EXPECT_FALSE(builder.finished());
}

TEST_F(LoggedCommandIdentifiersBuilderTest, StoppingABuildThatHasNotFinishedEndsItWithoutARecord) {
    for (int number = 1; number <= 3000; ++number) {
        append("order-" + std::to_string(number));
    }
    const std::vector<LoggedCommandIdentifiersBuilder::Segment> listed = segments();
    // The same segments many times over, so that the build is certainly still going when it is stopped.
    std::vector<LoggedCommandIdentifiersBuilder::Segment> many;
    for (int copy = 0; copy < 200; ++copy) {
        many.insert(many.end(), listed.begin(), listed.end());
    }
    LoggedCommandIdentifiersBuilder builder;
    builder.start(100000, std::move(many), identity_);
    builder.stop();
    EXPECT_FALSE(builder.finished()) << "a build that was stopped reported itself finished";
}

// The segments are listed when the WAL is opened, and a repair can cut the WAL back before the build reads
// them, deleting the later segments. The build passes over what it cannot read, finishes, and still holds
// every command left in the WAL.
TEST_F(LoggedCommandIdentifiersBuilderTest, SegmentsDeletedByCuttingTheWalBackArePassedOverAndTheBuildFinishes) {
    for (int number = 1; number <= 300; ++number) {
        append("order-" + std::to_string(number));
    }
    const std::vector<LoggedCommandIdentifiersBuilder::Segment> listed = segments();
    ASSERT_GT(listed.size(), 3U) << "the WAL should span several segments for this to test anything";
    constexpr int kept = 20;
    wal_->truncate_after(kept);
    int deleted = 0;
    for (const LoggedCommandIdentifiersBuilder::Segment& segment : listed) {
        deleted += std::filesystem::exists(segment.path) ? 0 : 1;
    }
    ASSERT_GT(deleted, 0) << "cutting the WAL back should have deleted at least one listed segment";

    LoggedCommandIdentifiersBuilder builder;
    builder.start(10000, listed, identity_);
    ASSERT_TRUE(wait_until_finished(builder)) << "the build did not finish";
    const LoggedCommandIdentifiers record = builder.take();

    for (int number = 1; number <= kept; ++number) {
        ASSERT_TRUE(record.find(LoggedCommandIdentifiers::identifier("order-" + std::to_string(number))).may_hold) << "order-" << number;
    }
    EXPECT_GE(builder.segments_not_read(), 1);
    EXPECT_FALSE(builder.last_segment_error().empty());
}

} // namespaces
