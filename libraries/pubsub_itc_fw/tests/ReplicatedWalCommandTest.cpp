// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ReplicatedWalCommandTest.cpp
 * @brief Tests of the replicated WAL's check of whether it already holds a command
 *        (docs/availability/one_check_against_sequencing_twice.md, section 3.2), with one instance running alone.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReplicatedWal.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include <lease_messages.hpp>
#include <replication_messages.hpp>

#include "TestCommandIdentity.hpp"

namespace pubsub_itc_fw::tests {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t small_segments = 4096;
constexpr int64_t the_whole_wal = std::numeric_limits<int64_t>::min();

// Running alone, the instance sends its peer nothing and asks no voter anything.
struct NoPeer : public ReplicationLinksInterface {
    [[nodiscard]] bool peer_connected() const override {
        return false;
    }
    void send_record_to_peer(const ReplicatedRecord& /*record*/) override {}
    void send_ack_to_peer(const ConnectionID& /*on*/, const WalAck& /*ack*/) override {}
    void send_position_request_to_peer(const LogPositionRequest& /*request*/) override {}
    void send_position_reply_to_peer(const LogPositionReply& /*reply*/) override {}
};

struct NoVoters : public LeaseLinksInterface {
    void send_request_to_peer(const LeaseRequest& /*request*/) override {}
    void send_request_to_third_voter(const LeaseRequest& /*request*/) override {}
    void send_grant(const ConnectionID& /*conn_id*/, const LeaseGrant& /*grant*/) override {}
    void send_refusal(const ConnectionID& /*conn_id*/, const LeaseRefusal& /*refusal*/) override {}
};

struct CheckEvents : public ReplicatedWalEventsInterface {
    int checking_reports{0};
    void role_changed(ReplicaRole /*from*/, ReplicaRole /*to*/, int32_t /*epoch*/) override {}
    void committed_through(int64_t /*seq_no*/) override {}
    void written_as_follower(int64_t /*seq_no*/, const WalEntryContents& /*entry*/) override {}
    void discarded_after(int64_t /*seq_no*/) override {}
    void granted_lease_to_peer(Clock::time_point /*at*/) override {}
    void read_at_open(int64_t /*seq_no*/, const WalEntryContents& /*entry*/) override {}
    void checking_for_commands_already_held(int64_t /*last_seq_no*/) override {
        ++checking_reports;
    }
};

class ReplicatedWalCommandTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = tests_common::make_scratch_directory("replicated_wal_command_test");
    }

    void TearDown() override {
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    // Opens an instance running alone, which leads at once, keeping a record of commands with room for
    // @p capacity, its numbers made by @p number_of.
    void open(size_t capacity = 1000, LoggedCommandIdentifiers::NumberFunction number_of = &LoggedCommandIdentifiers::identifier) {
        ReplicatedWalConfiguration configuration;
        configuration.owner_name = "TestReplica";
        configuration.wal_directory = dir_;
        configuration.wal_segment_size = small_segments;
        configuration.ha_enabled = false;
        configuration.instance_id = 1;
        configuration.command_identity = &identity_;
        configuration.identifiers_reserved = capacity;
        configuration.command_number = number_of;
        wal_ = std::make_unique<ReplicatedWal>(configuration, logger_, peer_, voters_, events_);
        static_cast<void>(wal_->open(now_));
        ASSERT_EQ(wal_->role(), ReplicaRole::leader);
    }

    // Builds the record of commands from the WAL as it stands, and waits until it is taken over.
    void build_record() {
        wal_->start_building_record_of_commands();
        const auto deadline = Clock::now() + std::chrono::seconds{10};
        while (wal_->record_of_commands() == nullptr) {
            ASSERT_LT(Clock::now(), deadline) << "the record of commands was not built";
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            wal_->take_record_of_commands_if_built();
        }
    }

    void write(const std::string& identity, int16_t pdu_id = test_command_pdu_id) {
        const std::vector<uint8_t> bytes = test_command_bytes(identity);
        static_cast<void>(wal_->append(pdu_id, bytes.data(), bytes.size(), ++wall_time_ns_, now_));
    }

    std::string dir_;
    std::vector<std::string> log_;
    QuillLogger logger_{FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    NoPeer peer_;
    NoVoters voters_;
    CheckEvents events_;
    TestCommandIdentity identity_;
    Clock::time_point now_{};
    int64_t wall_time_ns_{0};
    std::unique_ptr<ReplicatedWal> wal_;
};

} // namespaces

TEST_F(ReplicatedWalCommandTest, ACommandWrittenIsProvedByReadingItsOneRecordAndOneNeverWrittenIsNotHeld) {
    open();
    build_record();
    write("MEMBER/order-1");
    write("MEMBER/order-2");
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_FALSE(wal_->holds_command("MEMBER/order-3", the_whole_wal));
    EXPECT_EQ(wal_->command_check_counts().proved_by_reading, 1);
    EXPECT_EQ(wal_->command_check_counts().not_held_by_number, 1);
    EXPECT_EQ(wal_->command_check_counts().searched, 0);
    EXPECT_FALSE(wal_->command_search_in_use());
}

// Every command, in every segment, including one written at the start of a new segment because it did not fit
// in the rest of the last, is found at the position the WAL noted as it wrote it.
TEST_F(ReplicatedWalCommandTest, EveryCommandWrittenAcrossManySegmentsIsProvedByReadingWithoutASearch) {
    open();
    build_record();
    for (int number = 1; number <= 400; ++number) {
        write("MEMBER/order-" + std::to_string(number));
    }
    ASSERT_GT(wal_->wal().segments_on_disk().size(), 3U) << "the WAL should span several segments for this to test anything";
    for (int number = 1; number <= 400; ++number) {
        ASSERT_TRUE(wal_->holds_command("MEMBER/order-" + std::to_string(number), the_whole_wal)) << number;
    }
    EXPECT_EQ(wal_->command_check_counts().proved_by_reading, 400);
    EXPECT_EQ(wal_->command_check_counts().searched, 0);
}

// The commands already in the WAL when it is opened are read into the record by the building thread, with
// their positions.
TEST_F(ReplicatedWalCommandTest, CommandsInTheWalWhenItIsOpenedAreProvedByReading) {
    open();
    for (int number = 1; number <= 100; ++number) {
        write("MEMBER/order-" + std::to_string(number));
    }
    wal_.reset();
    open();
    build_record();
    for (int number = 1; number <= 100; ++number) {
        ASSERT_TRUE(wal_->holds_command("MEMBER/order-" + std::to_string(number), the_whole_wal)) << number;
    }
    EXPECT_EQ(wal_->command_check_counts().proved_by_reading, 100);
    EXPECT_EQ(wal_->command_check_counts().searched, 0);
}

// Two commands with the same number: the record keeps the first one's position, so a check of the second reads
// the first, finds a different command, and searches the WAL, which finds it.
TEST_F(ReplicatedWalCommandTest, OfTwoCommandsWithTheSameNumberBothAreFound) {
    open(1000, [](std::string_view /*identity*/) -> uint64_t { return 7; });
    build_record();
    write("MEMBER/first");
    write("MEMBER/second");
    EXPECT_TRUE(wal_->holds_command("MEMBER/first", the_whole_wal));
    EXPECT_TRUE(wal_->holds_command("MEMBER/second", the_whole_wal));
    EXPECT_FALSE(wal_->holds_command("MEMBER/third", the_whole_wal)) << "a third with the same number was never written";
    EXPECT_EQ(wal_->command_check_counts().proved_by_reading, 1);
    EXPECT_EQ(wal_->command_check_counts().other_command_at_position, 2);
    EXPECT_EQ(wal_->command_check_counts().searched, 2);
    EXPECT_EQ(wal_->find_command("MEMBER/first", the_whole_wal), std::optional<int64_t>{1}) << "proved by reading its record";
    EXPECT_EQ(wal_->find_command("MEMBER/second", the_whole_wal), std::optional<int64_t>{2}) << "found by the search";
}

TEST_F(ReplicatedWalCommandTest, TheRecordHoldingACommandIsNamed) {
    open();
    build_record();
    write("MEMBER/order-1");
    write("MEMBER/order-2");
    write("MEMBER/order-3");
    EXPECT_EQ(wal_->find_command("MEMBER/order-2", the_whole_wal), std::optional<int64_t>{2});
    EXPECT_EQ(wal_->find_command("MEMBER/order-3", the_whole_wal), std::optional<int64_t>{3});
    EXPECT_EQ(wal_->find_command("MEMBER/order-4", the_whole_wal), std::nullopt);
    EXPECT_EQ(wal_->command_check_counts().searched, 0);
}

TEST_F(ReplicatedWalCommandTest, AFullRecordFallsBackToASearchForACommandItDoesNotHold) {
    open(1);
    build_record();
    write("MEMBER/order-1");
    write("MEMBER/order-2");
    ASSERT_TRUE(wal_->record_of_commands()->full());
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-2", the_whole_wal)) << "written after the record filled, so found only by searching";
    EXPECT_FALSE(wal_->holds_command("MEMBER/order-3", the_whole_wal));
    EXPECT_EQ(wal_->command_check_counts().searched, 2);
    EXPECT_EQ(wal_->find_command("MEMBER/order-2", the_whole_wal), std::optional<int64_t>{2});
}

TEST_F(ReplicatedWalCommandTest, BeforeTheRecordIsBuiltTheWalIsSearched) {
    open();
    write("MEMBER/order-1");
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_FALSE(wal_->holds_command("MEMBER/order-2", the_whole_wal));
    EXPECT_EQ(wal_->command_check_counts().searched, 2);
    EXPECT_TRUE(wal_->command_search_in_use());
    EXPECT_GT(wal_->discard_command_search(), 0);
    EXPECT_FALSE(wal_->command_search_in_use());
}

// A command written while the record is being built is noted and added when the record is taken over.
TEST_F(ReplicatedWalCommandTest, ACommandWrittenWhileTheRecordIsBuiltIsAddedWhenItIsTakenOver) {
    open();
    wal_->start_building_record_of_commands();
    write("MEMBER/order-1");
    const auto deadline = Clock::now() + std::chrono::seconds{10};
    while (wal_->record_of_commands() == nullptr) {
        ASSERT_LT(Clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        wal_->take_record_of_commands_if_built();
    }
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_EQ(wal_->command_check_counts().proved_by_reading, 1);
}

TEST_F(ReplicatedWalCommandTest, ARecordThatIsNotACommandIsNotHeld) {
    open();
    build_record();
    write("MEMBER/order-1", test_other_pdu_id);
    EXPECT_FALSE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
}

// The leader says once in each leadership that it is checking for commands already held, and not for a
// command the record rules out.
TEST_F(ReplicatedWalCommandTest, CheckingForCommandsAlreadyHeldIsReportedOnceAndOnlyForAPossibleMatch) {
    open();
    build_record();
    write("MEMBER/order-1");
    EXPECT_FALSE(wal_->holds_command("MEMBER/never", the_whole_wal));
    EXPECT_EQ(events_.checking_reports, 0);
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_TRUE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
    EXPECT_EQ(events_.checking_reports, 1);
}

TEST_F(ReplicatedWalCommandTest, WithoutACommandIdentityNothingIsHeld) {
    ReplicatedWalConfiguration configuration;
    configuration.owner_name = "TestReplica";
    configuration.wal_directory = dir_;
    configuration.wal_segment_size = small_segments;
    configuration.ha_enabled = false;
    configuration.instance_id = 1;
    wal_ = std::make_unique<ReplicatedWal>(configuration, logger_, peer_, voters_, events_);
    static_cast<void>(wal_->open(now_));
    write("MEMBER/order-1");
    EXPECT_FALSE(wal_->holds_command("MEMBER/order-1", the_whole_wal));
}

} // namespaces
