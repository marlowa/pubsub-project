#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Static utility functions for filesystem operations.
 *
 * @ingroup utilities_subsystem
 *
 * Implementation note -- why POSIX mkdir instead of std::filesystem:
 *
 *   std::filesystem was introduced in C++17 but GCC 8.5 (shipped with
 *   RHEL 8) requires linking against a separate -lstdc++fs library for
 *   filesystem support, and has known bugs and missing functionality in
 *   its std::filesystem implementation. Rather than introduce a fragile
 *   link dependency and risk subtle runtime failures on RHEL 8, this
 *   class uses POSIX mkdir(2) and stat(2) directly. These are available
 *   on all POSIX platforms without any additional link flags and behave
 *   correctly on GCC 8.5 and all later compilers.
 *
 *   When RHEL 8 / GCC 8.5 support is no longer required, this
 *   implementation can be replaced with std::filesystem::create_directories
 *   without changing the public interface.
 */
class FileSystemUtils {
  public:
    /**
     * @brief Creates all directories in the given path that do not already
     *        exist, equivalent to `mkdir -p`.
     *
     * Splits the path on '/' and calls mkdir(2) on each component in turn.
     * EEXIST is silently ignored (the directory already exists). Any other
     * errno value is treated as a failure.
     *
     * @param[in] path The directory path to create. May be absolute or relative.
     *                 An empty path is a no-op and returns success.
     * @return An empty string on success, or a human-readable error description
     *         on failure. The description includes the failing path component
     *         and the system error string.
     */
    [[nodiscard]] static std::string make_directories(const std::string& path);

    /**
     * @brief The mount options of the filesystem holding @p path.
     *
     * Reads /proc/self/mounts and returns the options of the mount whose mount point is the
     * longest prefix of @p path -- the filesystem a file there actually lives on, since mount
     * points nest.
     *
     * This exists because a mount option can decide whether a component meets its latency
     * requirement, and nothing in a program's own configuration reveals it. Measured on
     * 2026-08-31: with the sequencer's log on a filesystem mounted `relatime`, appends over
     * 100 ms happened five times in 9.3 million and the reactor stalled for up to 845 ms; with
     * the same filesystem mounted `lazytime`, no append exceeded 10 ms in 9.3 million and the
     * longest stall was under a millisecond. See docs/operations/filesystem_requirements.md.
     *
     * @param[in] path A file or directory. Need not exist; what matters is which mount it
     *                 falls under.
     * @return The comma-separated option list, or an empty string if it could not be
     *         determined. An empty answer means "unknown", never "no options" -- a caller must
     *         not read it as the absence of a particular option.
     */
    [[nodiscard]] static std::string mount_options(const std::string& path);
};

} // namespaces
