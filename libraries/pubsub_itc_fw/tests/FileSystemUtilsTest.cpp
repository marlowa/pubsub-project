// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FileSystemUtils.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace pubsub_itc_fw::tests {

// mount_options() exists because a mount option can decide whether a component meets its
// latency requirement while appearing in no file this project holds. On 2026-08-31 the
// sequencer's log went from stalls of up to 845 ms to none over 10 ms across 9.3 million
// records, with nothing changed but `lazytime` on the filesystem holding it. The sequencer
// warns at startup when that option is absent, and this is what the warning rests on.

TEST(FileSystemUtilsTest, TheRootFilesystemHasOptions) {
    // Whatever else is true of a running Linux system, / is mounted and has options.
    const std::string options = FileSystemUtils::mount_options("/");
    EXPECT_FALSE(options.empty()) << "no options found for /, so nothing can be checked about any path";
    EXPECT_NE(options.find("rw"), std::string::npos) << "expected / to be writable, found [" << options << "]";
}

TEST(FileSystemUtilsTest, APathInsideTheScratchAreaReportsItsFilesystem) {
    const std::string dir = tests_common::make_scratch_directory("mount_options");
    const std::string options = FileSystemUtils::mount_options(dir);
    EXPECT_FALSE(options.empty()) << "no options found for " << dir;
    tests_common::remove_scratch_directory(dir);
}

TEST(FileSystemUtilsTest, APathThatDoesNotExistStillReportsAFilesystem) {
    // The question is which mount a path falls under, not whether anything is there yet. The
    // sequencer asks before its log directory has been created.
    const std::string absent = tests_common::scratch_path_that_does_not_exist("never_made");
    EXPECT_FALSE(FileSystemUtils::mount_options(absent).empty());
}

TEST(FileSystemUtilsTest, AnEmptyPathReportsNothingRatherThanGuessing) {
    EXPECT_TRUE(FileSystemUtils::mount_options("").empty());
}

TEST(FileSystemUtilsTest, TheAnswerComesFromTheLongestMatchingMountPoint) {
    // Mount points nest, so a path under a mounted subdirectory matches the root mount too.
    // Taking the first match rather than the longest would report the wrong filesystem, and
    // the option being looked for lives on the inner one.
    const std::string root = FileSystemUtils::mount_options("/");
    const std::string proc = FileSystemUtils::mount_options("/proc/self/mounts");

    ASSERT_FALSE(root.empty());
    ASSERT_FALSE(proc.empty());
    // /proc is a separate mount from /, so if the longest match is being used these differ.
    // If they are equal the code returned the root mount for a path that is not on it.
    EXPECT_NE(root, proc) << "both reported [" << root << "], so the first matching mount was used rather than the longest";
}

} // namespaces
