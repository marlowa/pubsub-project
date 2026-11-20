#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief This process's limit on how much memory it may lock (RLIMIT_MEMLOCK), as it is when read, and whether it
 *        allows a component to lock what it needs.
 *
 * A component that locks its large tables into memory at start (docs/venue/the_engine_holds_its_memory_from_the_start.md)
 * reads the limit first, so that a limit too low is reported with the figure needed before any time is spent locking,
 * rather than as a failure part of the way through.
 */
class LockedMemoryLimit {
  public:
    /// The limit as it is now.
    [[nodiscard]] static LockedMemoryLimit current();

    /**
     * @brief Whether this process may lock @p bytes.
     *
     * True when the limit could not be read, so that locking itself says what is wrong; when it is unlimited; when it
     * is at least @p bytes; or when the process runs as root, which may lock memory whatever its limit.
     */
    [[nodiscard]] bool allows(size_t bytes) const;

    /// The limit in words for a log: "unknown", "unlimited", or a number of megabytes such as "4096 MB".
    [[nodiscard]] std::string text() const;

  private:
    bool known_{false};
    bool unlimited_{false};
    uint64_t bytes_{0};
    bool runs_as_root_{false};
};

/// What an errno from mlock() means, in words an operator can act on.
[[nodiscard]] std::string why_locking_failed(int error);

/// What an operator does about a refusal to start for want of locked memory, the same for every component.
inline constexpr const char* locked_memory_remedy =
    "The limit for the user the venue runs as is set in /etc/security/limits.d (memlock, in kB) or by whatever starts the "
    "launcher (ulimit -l); it must be at least the figure above, and the machine must have that much memory to spare.";

/// The number of bytes in a megabyte, as the logs that state these figures count them.
inline constexpr size_t bytes_in_a_megabyte = 1024UL * 1024UL;

} // namespaces
