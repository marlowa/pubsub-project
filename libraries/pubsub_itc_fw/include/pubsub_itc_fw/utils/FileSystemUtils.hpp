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
};

} // namespace pubsub_itc_fw
