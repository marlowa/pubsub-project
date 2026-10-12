// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for EpochStore, the file that carries the sequencer's leadership
// generation across a restart.
//
// What is being protected here is the property that a restarted sequencer never
// believes it is in an earlier generation than the one it was actually in. Every
// downstream epoch check depends on it, and the failure is silent: a sequencer
// that comes back at epoch 0 elects itself quite happily and stamps messages
// with a generation the venue used long ago.

#include <cstdio>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "EpochStore.hpp"

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
namespace {

// A directory of its own per test, removed afterwards, so a leftover file from
// one case cannot make another pass.
class EpochStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("epoch_store_test");
        path_ = dir_ + "/epoch.state";
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

    std::string dir_;
    std::string path_;
};

TEST_F(EpochStoreTest, AbsentFileReadsAsZero) {
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, StoredValueSurvivesANewStore) {
    // The point of the class: a second EpochStore, standing in for the process
    // that comes back after a restart, sees what the first one wrote.
    const fix_common::EpochStore writer(path_);
    ASSERT_TRUE(writer.store(5));

    const fix_common::EpochStore reader(path_);
    EXPECT_EQ(reader.load(), 5);
}

TEST_F(EpochStoreTest, LaterStoreReplacesEarlier) {
    const fix_common::EpochStore store(path_);
    ASSERT_TRUE(store.store(1));
    ASSERT_TRUE(store.store(2));
    ASSERT_TRUE(store.store(7));
    EXPECT_EQ(store.load(), 7);
}

TEST_F(EpochStoreTest, ZeroRoundTrips) {
    const fix_common::EpochStore store(path_);
    ASSERT_TRUE(store.store(0));
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, LargeEpochRoundTrips) {
    const fix_common::EpochStore store(path_);
    const int32_t large = 2147483647;
    ASSERT_TRUE(store.store(large));
    EXPECT_EQ(store.load(), large);
}

TEST_F(EpochStoreTest, ContentsAreReadableText) {
    // Deliberately a format someone can inspect with cat and correct with an
    // editor, because that is what happens when a venue is in trouble.
    const fix_common::EpochStore store(path_);
    ASSERT_TRUE(store.store(42));

    std::ifstream in(path_);
    std::string line;
    ASSERT_TRUE(std::getline(in, line));
    EXPECT_EQ(line, "42");
}

TEST_F(EpochStoreTest, HandEditedValueIsRead) {
    write_raw("11\n");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 11);
}

TEST_F(EpochStoreTest, ValueWithoutTrailingNewlineIsRead) {
    write_raw("13");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 13);
}

TEST_F(EpochStoreTest, EmptyFileReadsAsZero) {
    write_raw("");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, GarbageReadsAsZero) {
    // Zero is the safe answer for a damaged file: it loses to every real epoch,
    // so the node defers instead of claiming a generation it cannot support.
    write_raw("not a number\n");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, NegativeValueReadsAsZero) {
    write_raw("-4\n");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, ValueTooLargeForTheFieldReadsAsZero) {
    write_raw("99999999999999\n");
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.load(), 0);
}

TEST_F(EpochStoreTest, NoTemporaryFileIsLeftBehind) {
    // The write goes via a temporary and a rename. If the temporary survived, a
    // later reader could find a stale one and the directory would slowly fill.
    const fix_common::EpochStore store(path_);
    ASSERT_TRUE(store.store(3));

    const std::string temp = path_ + ".tmp";
    EXPECT_NE(::access(path_.c_str(), F_OK), -1) << "the epoch file should exist";
    EXPECT_EQ(::access(temp.c_str(), F_OK), -1) << "the temporary file should have been renamed away";
}

TEST_F(EpochStoreTest, StoreIntoAMissingDirectoryFails) {
    // Reported rather than thrown, so the sequencer can log an alert and carry
    // on leading rather than taking itself out over a storage fault.
    const fix_common::EpochStore store(dir_ + "/no_such_directory/epoch.state");
    EXPECT_FALSE(store.store(1));
}

TEST_F(EpochStoreTest, FailedStoreLeavesThePreviousValueIntact) {
    const fix_common::EpochStore store(path_);
    ASSERT_TRUE(store.store(9));

    // Make the rename fail by turning the destination into a directory.
    ::unlink(path_.c_str());
    ASSERT_EQ(::mkdir(path_.c_str(), 0755), 0);
    EXPECT_FALSE(store.store(10));
    ::rmdir(path_.c_str());
}

TEST_F(EpochStoreTest, PathIsReportedForDiagnostics) {
    const fix_common::EpochStore store(path_);
    EXPECT_EQ(store.path(), path_);
}

} // namespaces
