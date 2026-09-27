// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for LeasePromiseStore, the file that carries an instance's promise as a voter across a
// restart of its process, but not across a reboot of the machine.

#include <chrono>
#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

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
    ASSERT_TRUE(store.record(2, until));
    const auto record = store.load();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->promised_to, 2);
    EXPECT_EQ(record->until, until);
}

TEST_F(LeasePromiseStoreTest, NoPromiseIsRecordedAsZero) {
    fix_common::LeasePromiseStore store(path_, "boot-a");
    ASSERT_TRUE(store.record(0, Clock::time_point{} + milliseconds{5}));
    const auto record = store.load();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->promised_to, 0);
}

TEST_F(LeasePromiseStoreTest, ARecordFromAnotherBootIsNotBelieved) {
    ASSERT_TRUE(fix_common::LeasePromiseStore(path_, "boot-a").record(2, Clock::time_point{} + milliseconds{5}));
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
    EXPECT_FALSE(store.record(2, Clock::time_point{} + milliseconds{5}));
    EXPECT_FALSE(store.load().has_value());
}

TEST_F(LeasePromiseStoreTest, TheBootIdOfThisMachineCanBeRead) {
    EXPECT_FALSE(fix_common::LeasePromiseStore::current_boot_id().empty());
}
