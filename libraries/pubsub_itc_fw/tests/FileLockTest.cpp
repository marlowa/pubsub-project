// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FileLock.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
using namespace pubsub_itc_fw;

namespace {

// True if the file can be flock'd exclusively right now (i.e. nothing else holds
// the lock). Uses a separate fd with LOCK_NB so it never blocks the test.
bool file_is_lockable(const std::string& path) {
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return false;
    }
    const int result = ::flock(fd, LOCK_EX | LOCK_NB);
    if (result == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return true;
    }
    ::close(fd);
    return false;
}

} // namespaces

class FileLockTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("pubsub_file_lock_test");
        path_ = dir_ + "/lock";
        other_path_ = dir_ + "/other.lock";
    }

    void TearDown() override {
        pubsub_itc_fw::tests_common::remove_scratch_directory(dir_);
    }

    std::string dir_{};
    std::string path_{};
    std::string other_path_{};
};

TEST_F(FileLockTest, ConstructionAcquiresAnExclusiveLock) {
    FileLock lock(path_);

    EXPECT_FALSE(file_is_lockable(path_));
}

TEST_F(FileLockTest, DestructionReleasesTheLock) {
    {
        FileLock lock(path_);
        EXPECT_FALSE(file_is_lockable(path_));
    }

    EXPECT_TRUE(file_is_lockable(path_));
}

TEST_F(FileLockTest, MoveConstructionKeepsTheLockHeld) {
    FileLock original(path_);
    FileLock moved(std::move(original));

    // The moved-to object holds the lock; the moved-from object (fd_ = -1) releasing
    // does not drop it.
    EXPECT_FALSE(file_is_lockable(path_));
}

TEST_F(FileLockTest, MoveConstructedLockReleasesOnDestruction) {
    {
        FileLock original(path_);
        FileLock moved(std::move(original));
        EXPECT_FALSE(file_is_lockable(path_));
    }

    EXPECT_TRUE(file_is_lockable(path_));
}

TEST_F(FileLockTest, MoveAssignmentReleasesOldLockAndTakesTheNew) {
    FileLock a(path_);
    FileLock b(other_path_);
    ASSERT_FALSE(file_is_lockable(path_));
    ASSERT_FALSE(file_is_lockable(other_path_));

    b = std::move(a);

    // b released its original lock and now holds path_'s.
    EXPECT_TRUE(file_is_lockable(other_path_));
    EXPECT_FALSE(file_is_lockable(path_));
}

TEST_F(FileLockTest, ThrowsWhenLockFileCannotBeOpened) {
    EXPECT_THROW(FileLock("/nonexistent_directory_zzz/pubsub.lock"), PubSubItcException);
}
