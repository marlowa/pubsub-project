// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/CpuRegistry.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
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

    /*
    The number of cores the machine actually has, read without going anywhere near
    CpuRegistry. Tests below use it to decide whether the machine can host the
    test; anything else -- asking the registry how many cores it managed to claim
    and skipping when the answer is none -- makes the test pass whether the
    registry works or not, which is the failure the whole file is guarding against.
    */
    static unsigned int machine_core_count() {
        return std::thread::hardware_concurrency();
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

TEST_F(CpuRegistryTest, ThrowsWhenTheRegistryPathIsEmpty) {
    EXPECT_THROW(CpuRegistry("", lock_path_), PreconditionAssertion);
}

TEST_F(CpuRegistryTest, ThrowsWhenTheLockFilePathIsEmpty) {
    EXPECT_THROW(CpuRegistry(shm_path_, ""), PreconditionAssertion);
}

TEST_F(CpuRegistryTest, TwoRegistriesOnOneFileNeverClaimTheSameCore) {
    CpuRegistry first(shm_path_, lock_path_);
    CpuRegistry second(shm_path_, lock_path_);

    if (machine_core_count() < 2) {
        GTEST_SKIP() << "machine has fewer than two cores";
    }

    const AvailableCpuVector first_claim = first.claim_cpus(2, false);
    const AvailableCpuVector second_claim = second.claim_cpus(2, false);

    ASSERT_FALSE(first_claim.empty()) << "a machine with cores claimed none";
    ASSERT_FALSE(second_claim.empty()) << "a machine with cores claimed none";

    std::set<int> first_cores;
    for (const CpuAssignment& assignment : first_claim) {
        first_cores.insert(assignment.cpu_id.get_value());
    }
    for (const CpuAssignment& assignment : second_claim) {
        EXPECT_EQ(first_cores.count(assignment.cpu_id.get_value()), static_cast<size_t>(0))
            << "core " << assignment.cpu_id.get_value() << " was handed to both registries";
    }
}

TEST_F(CpuRegistryTest, ACoreHeldByALiveProcessIsNotReissued) {
    if (machine_core_count() < 2) {
        GTEST_SKIP() << "machine has fewer than two cores";
    }

    int child_to_parent[2]{-1, -1}; // NOLINT(cppcoreguidelines-pro-type-member-init)
    int parent_to_child[2]{-1, -1}; // NOLINT(cppcoreguidelines-pro-type-member-init)
    ASSERT_EQ(::pipe(child_to_parent), 0);
    ASSERT_EQ(::pipe(parent_to_child), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        ::close(child_to_parent[0]);
        ::close(parent_to_child[1]);
        CpuRegistry registry(shm_path_, lock_path_);
        const AvailableCpuVector claimed = registry.claim_cpus(1, false);
        const int held_core = claimed.empty() ? -1 : claimed[0].cpu_id.get_value();
        const ssize_t written = ::write(child_to_parent[1], &held_core, sizeof(held_core));
        // Hold the claim until the parent has looked, or until it dies and closes
        // the pipe. The read returning 0 on EOF is what stops this outliving the test.
        char release_signal = 0;
        const ssize_t read_back = ::read(parent_to_child[0], &release_signal, sizeof(release_signal));
        ::_exit((written > 0 && read_back >= 0) ? 0 : 1);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    int child_core = -1;
    const ssize_t received = ::read(child_to_parent[0], &child_core, sizeof(child_core));
    EXPECT_EQ(received, static_cast<ssize_t>(sizeof(child_core)));
    EXPECT_GE(child_core, 0) << "child process on a multi-core machine claimed nothing";

    if (child_core >= 0) {
        // Ask for far more cores than exist so the answer covers the whole machine:
        // if the child's core is anywhere in the pool, it will be in this result.
        CpuRegistry registry(shm_path_, lock_path_);
        const AvailableCpuVector claimed = registry.claim_cpus(SharedCoreRegistryLayout::max_system_cores, false);
        EXPECT_FALSE(claimed.empty()) << "a machine with cores claimed none";
        for (const CpuAssignment& assignment : claimed) {
            EXPECT_NE(assignment.cpu_id.get_value(), child_core) << "a core held by a live process was reissued";
        }
    }

    ::close(parent_to_child[1]); // lets the child finish
    ::close(child_to_parent[0]);
    int child_status = 0;
    EXPECT_EQ(::waitpid(child, &child_status, 0), child);
}

TEST_F(CpuRegistryTest, CoresLeakedByADeadProcessAreReclaimed) {
    if (machine_core_count() < 1) {
        GTEST_SKIP() << "machine reports no cores";
    }

    int child_to_parent[2]{-1, -1}; // NOLINT(cppcoreguidelines-pro-type-member-init)
    ASSERT_EQ(::pipe(child_to_parent), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        ::close(child_to_parent[0]);
        // Claim every core, then die without releasing -- the crash this models.
        // _exit() does not unwind, so the registry destructor never runs and the
        // entries stay in the file owned by a pid that no longer exists.
        CpuRegistry registry(shm_path_, lock_path_);
        const AvailableCpuVector claimed = registry.claim_cpus(SharedCoreRegistryLayout::max_system_cores, false);
        const int claimed_count = static_cast<int>(claimed.size());
        const ssize_t written = ::write(child_to_parent[1], &claimed_count, sizeof(claimed_count));
        ::_exit(written > 0 ? 0 : 1);
    }

    ::close(child_to_parent[1]);

    int child_claim_count = 0;
    const ssize_t received = ::read(child_to_parent[0], &child_claim_count, sizeof(child_claim_count));
    ASSERT_EQ(received, static_cast<ssize_t>(sizeof(child_claim_count)));
    ::close(child_to_parent[0]);

    int child_status = 0;
    ASSERT_EQ(::waitpid(child, &child_status, 0), child);

    ASSERT_GT(child_claim_count, 0) << "child process on a machine with cores claimed nothing";

    // Every core is now recorded against a dead pid. Without the stale-entry
    // compaction in claim_cpus(), all of them still read as busy and this returns
    // nothing -- a crashed component would strand its cores permanently.
    CpuRegistry registry(shm_path_, lock_path_);
    const AvailableCpuVector reclaimed = registry.claim_cpus(1, false);
    EXPECT_FALSE(reclaimed.empty()) << "cores left behind by a dead process were never reclaimed";
}

TEST_F(CpuRegistryTest, ReleaseReturnsTheCoreToThePool) {
    if (machine_core_count() < 1) {
        GTEST_SKIP() << "machine reports no cores";
    }
    CpuRegistry registry(shm_path_, lock_path_);

    const AvailableCpuVector first_claim = registry.claim_cpus(1, false);
    ASSERT_FALSE(first_claim.empty()) << "a machine with cores claimed none";
    const int first_core = first_claim[0].cpu_id.get_value();

    registry.release_cpus();

    // Claiming takes cores in enumeration order, so a genuinely released core is
    // the one handed back. Were release a no-op, the core would still read as busy
    // and a different one would arrive.
    const AvailableCpuVector second_claim = registry.claim_cpus(1, false);
    ASSERT_FALSE(second_claim.empty());
    EXPECT_EQ(second_claim[0].cpu_id.get_value(), first_core);
}

TEST_F(CpuRegistryTest, DestructionReleasesClaimedCores) {
    if (machine_core_count() < 1) {
        GTEST_SKIP() << "machine reports no cores";
    }
    int held_core = -1;
    {
        CpuRegistry holder(shm_path_, lock_path_);
        const AvailableCpuVector claimed = holder.claim_cpus(1, false);
        ASSERT_FALSE(claimed.empty()) << "a machine with cores claimed none";
        held_core = claimed[0].cpu_id.get_value();
    }

    CpuRegistry successor(shm_path_, lock_path_);
    const AvailableCpuVector claimed = successor.claim_cpus(1, false);
    ASSERT_FALSE(claimed.empty());
    EXPECT_EQ(claimed[0].cpu_id.get_value(), held_core);
}
