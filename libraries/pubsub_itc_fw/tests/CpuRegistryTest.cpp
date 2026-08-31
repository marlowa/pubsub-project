// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdlib>
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
#include <pubsub_itc_fw/StringUtils.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
using namespace pubsub_itc_fw;

class CpuRegistryTest : public ::testing::Test {
  protected:
    /*
    Both files go in a directory unique to this test, not at a fixed machine-wide
    name. A fixed name means two runs of the suite -- two developers on a shared
    box, or one developer running it twice -- share a registry and interfere with
    each other's claims, which is the same mistake the production configuration
    used to make by putting the registry in /dev/shm.
    */
    void SetUp() override {
        directory_ = pubsub_itc_fw::tests_common::make_scratch_directory("pubsub_cpu_registry_test");
        shm_path_ = directory_ + "/registry";
        lock_path_ = directory_ + "/registry.lock";
    }

    void TearDown() override {
        ::unlink(shm_path_.c_str());
        ::unlink(lock_path_.c_str());
        if (!directory_.empty()) {
            ::rmdir(directory_.c_str());
        }
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

    std::string directory_{};
    std::string shm_path_{};
    std::string lock_path_{};
};

TEST_F(CpuRegistryTest, ConstructionCreatesAndSizesTheSharedFile) {
    CpuRegistry registry(shm_path_, lock_path_);

    struct stat file_status {};
    ASSERT_EQ(::stat(shm_path_.c_str(), &file_status), 0) << "registry file was not created";
    EXPECT_EQ(static_cast<size_t>(file_status.st_size), sizeof(SharedCoreRegistryLayout));
}

TEST_F(CpuRegistryTest, ThrowsWhenSharedFileCannotBeOpened) {
    EXPECT_THROW(CpuRegistry("/proc/nonexistent_directory/registry", lock_path_), PubSubItcException);
}

TEST_F(CpuRegistryTest, ThrowsWhenTheRegistryPathIsEmpty) {
    EXPECT_THROW(CpuRegistry("", lock_path_), PreconditionAssertion);
}

TEST_F(CpuRegistryTest, ThrowsWhenTheLockFilePathIsEmpty) {
    EXPECT_THROW(CpuRegistry(shm_path_, ""), PreconditionAssertion);
}

TEST_F(CpuRegistryTest, RecordingNothingIsAccepted) {
    CpuRegistry registry(shm_path_, lock_path_);

    const auto [accepted, message] = registry.record_assignment({});
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(message.empty());
}

TEST_F(CpuRegistryTest, AFreshRegistryReportsNoCollision) {
    CpuRegistry registry(shm_path_, lock_path_);

    const auto [accepted, message] = registry.record_assignment({CpuId{0}, CpuId{1}});
    EXPECT_TRUE(accepted) << message;
    EXPECT_TRUE(message.empty());
}

/*
Re-recording is what a component does when it restarts, and the layout deliberately
gives it the same cores it had before. Reporting that as a collision with itself
would make every restart look like a misconfiguration.
*/
TEST_F(CpuRegistryTest, AProcessDoesNotCollideWithItself) {
    CpuRegistry registry(shm_path_, lock_path_);

    const auto [first_accepted, first_message] = registry.record_assignment({CpuId{3}});
    ASSERT_TRUE(first_accepted) << first_message;

    const auto [second_accepted, second_message] = registry.record_assignment({CpuId{3}});
    EXPECT_TRUE(second_accepted) << second_message;
}

/*
The case the declared layout cannot catch on its own: two installations on one
machine, each with its own layout file, each self-consistent, both handing out the
same core. Only a shared record spanning both can see it.
*/
TEST_F(CpuRegistryTest, ACoreHeldByAnotherLiveProcessIsReportedAsACollision) {
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
        const auto [accepted, message] = registry.record_assignment({CpuId{7}});
        const char recorded = accepted ? 1 : 0;
        const ssize_t written = ::write(child_to_parent[1], &recorded, sizeof(recorded));
        // Stay alive, and stay the owner, until the parent has looked. The read
        // returning 0 at EOF is what stops this outliving the test.
        char release_signal = 0;
        const ssize_t read_back = ::read(parent_to_child[0], &release_signal, sizeof(release_signal));
        ::_exit((written > 0 && read_back >= 0) ? 0 : 1);
    }

    ::close(child_to_parent[1]);
    ::close(parent_to_child[0]);

    char child_recorded = 0;
    ASSERT_EQ(::read(child_to_parent[0], &child_recorded, sizeof(child_recorded)), static_cast<ssize_t>(sizeof(child_recorded)));
    EXPECT_EQ(child_recorded, 1);

    CpuRegistry registry(shm_path_, lock_path_);
    const auto [accepted, message] = registry.record_assignment({CpuId{7}});
    EXPECT_FALSE(accepted) << "a core held by another live process was not reported";
    EXPECT_NE(message.find("CPU 7"), std::string::npos) << message;
    EXPECT_NE(message.find(std::to_string(child)), std::string::npos) << "the message should name the process holding the core: " << message;

    ::close(parent_to_child[1]); // lets the child finish
    ::close(child_to_parent[0]);
    int child_status = 0;
    EXPECT_EQ(::waitpid(child, &child_status, 0), child);
}

/*
A process that died without releasing must not hold a core hostage. Without the
eviction pass in record_assignment() a single crash would make that core look
permanently contended.
*/
TEST_F(CpuRegistryTest, ACoreLeftByADeadProcessIsNotACollision) {
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        CpuRegistry registry(shm_path_, lock_path_);
        const auto [accepted, message] = registry.record_assignment({CpuId{9}});
        // Leave the entry behind deliberately: _exit skips the destructor, which
        // is exactly what a crash does.
        ::_exit(accepted ? 0 : 1);
    }

    int child_status = 0;
    ASSERT_EQ(::waitpid(child, &child_status, 0), child);
    ASSERT_TRUE(WIFEXITED(child_status));
    ASSERT_EQ(WEXITSTATUS(child_status), 0) << "child failed to record its core";

    CpuRegistry registry(shm_path_, lock_path_);
    const auto [accepted, message] = registry.record_assignment({CpuId{9}});
    EXPECT_TRUE(accepted) << "a core left by a dead process was treated as contended: " << message;
}

TEST_F(CpuRegistryTest, ReleaseIsIdempotent) {
    CpuRegistry registry(shm_path_, lock_path_);
    const auto [accepted, message] = registry.record_assignment({CpuId{1}});
    ASSERT_TRUE(accepted) << message;

    EXPECT_NO_THROW(registry.release_cpus());
    EXPECT_NO_THROW(registry.release_cpus());
}

TEST_F(CpuRegistryTest, MoveConstructionLeavesSourceInert) {
    CpuRegistry source(shm_path_, lock_path_);
    CpuRegistry moved(std::move(source));

    // The moved-from registry has no mapping, so it records nothing and reports
    // no collision rather than dereferencing a null layout.
    const auto [source_accepted, source_message] = source.record_assignment({CpuId{2}}); // NOLINT(bugprone-use-after-move)
    EXPECT_TRUE(source_accepted) << source_message;

    const auto [moved_accepted, moved_message] = moved.record_assignment({CpuId{2}});
    EXPECT_TRUE(moved_accepted) << moved_message;
}
