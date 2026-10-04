// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for LeasePromiseStore, the file that carries an instance's promise as a voter across a
// restart of its process, but not across a reboot of the machine.

#include <chrono>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include <LeaderStatement.hpp>
#include <LeasePromiseRecord.hpp>
#include <LeasePromiseStore.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// A directory of its own per test, removed afterwards, so a leftover record from one case cannot make
// another pass.
class LeasePromiseStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("lease_promise_store_test");
        path_ = dir_ + "/lease_promise.state";
    }

    void TearDown() override {
        ::unlink(path_.c_str());
        ::unlink((path_ + ".tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    std::string dir_;
    std::string path_;
};

} // un-named namespace

TEST_F(LeasePromiseStoreTest, AStoredPromiseIsReadBackDuringTheSameBoot) {
    fix_common::LeasePromiseStore store(path_, "boot-a");
    const Clock::time_point until = Clock::time_point{} + milliseconds{123456};
    ASSERT_TRUE(store.record(fix_common::LeasePromiseRecord{2, until, {}}));
    const auto record = store.load();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->promised_to, 2);
    EXPECT_EQ(record->until, until);
}

TEST_F(LeasePromiseStoreTest, NoPromiseIsRecordedAsZero) {
    fix_common::LeasePromiseStore store(path_, "boot-a");
    ASSERT_TRUE(store.record(fix_common::LeasePromiseRecord{0, Clock::time_point{} + milliseconds{5}, {}}));
    const auto record = store.load();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->promised_to, 0);
}

TEST_F(LeasePromiseStoreTest, ARecordFromAnotherBootIsNotBelieved) {
    ASSERT_TRUE(fix_common::LeasePromiseStore(path_, "boot-a").record(fix_common::LeasePromiseRecord{2, Clock::time_point{} + milliseconds{5}, {}}));
    EXPECT_FALSE(fix_common::LeasePromiseStore(path_, "boot-b").load().has_value());
}

TEST_F(LeasePromiseStoreTest, AMissingRecordReadsAsNone) {
    EXPECT_FALSE(fix_common::LeasePromiseStore(path_, "boot-a").load().has_value());
}

TEST_F(LeasePromiseStoreTest, ADamagedRecordReadsAsNone) {
    std::ofstream(path_) << "boot-a two lots\n";
    EXPECT_FALSE(fix_common::LeasePromiseStore(path_, "boot-a").load().has_value());
}

TEST_F(LeasePromiseStoreTest, WithoutABootIdNothingIsRecordedOrBelieved) {
    fix_common::LeasePromiseStore store(path_, "");
    EXPECT_FALSE(store.record(fix_common::LeasePromiseRecord{2, Clock::time_point{} + milliseconds{5}, {}}));
    EXPECT_FALSE(store.load().has_value());
}

TEST_F(LeasePromiseStoreTest, TheBootIdOfThisMachineCanBeRead) {
    EXPECT_FALSE(fix_common::LeasePromiseStore::current_boot_id().empty());
}

TEST_F(LeasePromiseStoreTest, AStatementIsKeptWithThePromise) {
    fix_common::LeasePromiseStore store(path_, "boot-a");
    const fix_common::LeaderStatement statement{1, 5, 2, false};
    ASSERT_TRUE(store.record(fix_common::LeasePromiseRecord{2, Clock::time_point{} + milliseconds{5}, statement}));
    const auto record = store.load();
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->statement.same_as(statement));
    EXPECT_FALSE(record->statement.peer_may_lead);
    EXPECT_TRUE(store.load_statement().same_as(statement));
}

TEST_F(LeasePromiseStoreTest, AStatementIsBelievedAfterAReboot) {
    // A statement does not depend on the clock, and an instance that has recorded that its peer may not
    // lead must go on refusing that peer after its machine reboots.
    const fix_common::LeaderStatement statement{2, 9, 1, false};
    ASSERT_TRUE(fix_common::LeasePromiseStore(path_, "boot-a").record(fix_common::LeasePromiseRecord{1, Clock::time_point{} + milliseconds{5}, statement}));
    const fix_common::LeasePromiseStore after_reboot(path_, "boot-b");
    EXPECT_FALSE(after_reboot.load().has_value());
    EXPECT_TRUE(after_reboot.load_statement().same_as(statement));
    EXPECT_EQ(after_reboot.load_statement().instance_that_may_not_lead(), 1);
}

TEST_F(LeasePromiseStoreTest, ARecordWithoutAStatementReadsAsHoldingNone) {
    std::ofstream(path_) << "boot-a 2 5\n";
    const auto record = fix_common::LeasePromiseStore(path_, "boot-a").load();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->promised_to, 2);
    EXPECT_EQ(record->statement.leader_id, 0);
}

TEST_F(LeasePromiseStoreTest, ARecordWithADamagedStatementReadsAsNone) {
    std::ofstream(path_) << "boot-a 2 5 3 1 1 0\n";
    EXPECT_FALSE(fix_common::LeasePromiseStore(path_, "boot-a").load().has_value());
    EXPECT_EQ(fix_common::LeasePromiseStore(path_, "boot-a").load_statement().leader_id, 0);
}
