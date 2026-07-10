// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/CpuRegistry.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

using namespace pubsub_itc_fw;

class CpuRegistryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        shm_path_ = "/dev/shm/pubsub_cpu_registry_test.shm";
        lock_path_ = "/tmp/pubsub_cpu_registry_test.lock";
        ::unlink(shm_path_.c_str());
        ::unlink(lock_path_.c_str());
    }

    void TearDown() override {
        ::unlink(shm_path_.c_str());
        ::unlink(lock_path_.c_str());
    }

    std::string shm_path_{};
    std::string lock_path_{};
};

TEST_F(CpuRegistryTest, ConstructionCreatesAndSizesTheSharedFile) {
    CpuRegistry registry(shm_path_, lock_path_);

    struct stat st {};
    ASSERT_EQ(::stat(shm_path_.c_str(), &st), 0);
    EXPECT_GE(st.st_size, static_cast<off_t>(sizeof(SharedCoreRegistryLayout)));
}

TEST_F(CpuRegistryTest, ClaimReturnsAtMostRequestedCount) {
    CpuRegistry registry(shm_path_, lock_path_);

    const AvailableCpuVector claimed = registry.claim_cpus(2, false);

    EXPECT_LE(claimed.size(), static_cast<size_t>(2));
}

TEST_F(CpuRegistryTest, ClaimZeroReturnsEmpty) {
    CpuRegistry registry(shm_path_, lock_path_);

    EXPECT_TRUE(registry.claim_cpus(0, false).empty());
}

TEST_F(CpuRegistryTest, ClaimWithReserveCpu0Runs) {
    CpuRegistry registry(shm_path_, lock_path_);

    const AvailableCpuVector claimed = registry.claim_cpus(1, true);

    EXPECT_LE(claimed.size(), static_cast<size_t>(1));
    for (const CpuAssignment& assignment : claimed) {
        EXPECT_NE(assignment.cpu_id.get_value(), 0);
    }
}

TEST_F(CpuRegistryTest, ReleaseIsIdempotent) {
    CpuRegistry registry(shm_path_, lock_path_);

    const AvailableCpuVector claimed = registry.claim_cpus(1, false);
    EXPECT_LE(claimed.size(), static_cast<size_t>(1));

    registry.release_cpus();
    registry.release_cpus(); // second call is a no-op, must not crash
    SUCCEED();
}

TEST_F(CpuRegistryTest, ThrowsWhenSharedFileCannotBeOpened) {
    EXPECT_THROW(CpuRegistry("/nonexistent_directory_zzz/registry.shm", lock_path_), PubSubItcException);
}

TEST_F(CpuRegistryTest, MoveConstructionLeavesSourceInert) {
    CpuRegistry source(shm_path_, lock_path_);
    CpuRegistry moved(std::move(source));

    // The moved-from registry has no mapping, so a claim returns nothing and does
    // not crash.
    EXPECT_TRUE(source.claim_cpus(1, false).empty()); // NOLINT(bugprone-use-after-move)
    EXPECT_LE(moved.claim_cpus(1, false).size(), static_cast<size_t>(1));
}
