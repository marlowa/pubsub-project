#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw::tests_common {

/**
 * @brief A scratch directory for a test, on real disk, inside the project's own tree.
 *
 * Every test that needs a file on disk gets it from here. Two places tests must never use:
 *
 * `/dev/shm` is tmpfs. It has no journal, no block allocation and no delayed allocation, so a
 * test that asserts anything about filesystem behaviour there asserts it against a filesystem
 * that cannot exhibit the behaviour. The regression test for BUG-0070 -- that a new log segment
 * has its blocks already allocated -- was written against `/dev/shm` and passed without ever
 * exercising the mechanism it guards. A test that cannot fail is worse than no test, because it
 * is counted.
 *
 * `/tmp` is shared with every other user and process on the machine, is cleaned by policies
 * this project does not control, and on some hosts is itself tmpfs. Two runs of the suite can
 * collide there, and the failure looks like a bug in the code under test.
 *
 * The base directory is resolved in this order, and every candidate is on real disk:
 *
 *   1. `PUBSUB_TEST_SCRATCH_DIR`, set by `scripts/build.py` when it runs the suite.
 *   2. `PUBSUB_BUILD_DIR`, which the pybind11 harness already relies on, plus `/test_scratch`.
 *   3. The current working directory plus `/test_scratch`.
 *
 * @param name Short name for the directory, used to identify it if a run leaves one behind.
 * @return Absolute path to a newly created, empty directory. Throws std::runtime_error if one
 *         cannot be made, because a test silently writing somewhere unintended is the failure
 *         this class exists to prevent.
 */
[[nodiscard]] std::string make_scratch_directory(const std::string& name);

/**
 * @brief Removes a directory made by make_scratch_directory(), and everything in it.
 *
 * Does not throw: a test that has already reported its result should not then fail in teardown.
 */
void remove_scratch_directory(const std::string& path);

/**
 * @brief A path inside the scratch area that deliberately does not exist.
 *
 * For tests that need to name something absent -- an unreadable file, a missing directory.
 * Keeps those paths out of `/dev/shm` and `/tmp` as well.
 */
[[nodiscard]] std::string scratch_path_that_does_not_exist(const std::string& name);

} // namespaces
