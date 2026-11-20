#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <fstream>
#include <string>

#include <sys/resource.h>

namespace pubsub_itc_fw::tests_common {

/// The memory this process has locked, in kB, as the kernel reports it (VmLck in /proc/self/status).
inline size_t locked_kilobytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmLck:", 0) == 0) {
            return static_cast<size_t>(std::stoul(line.substr(6)));
        }
    }
    return 0;
}

/// Lowers this process's limit on locked memory for as long as it lives, and puts it back after. Lowering the soft limit
/// needs no privilege, and it can always be raised again as far as the hard limit.
class LockedMemoryLimitLowered {
  public:
    ~LockedMemoryLimitLowered() {
        ::setrlimit(RLIMIT_MEMLOCK, &saved_);
    }
    explicit LockedMemoryLimitLowered(rlim_t bytes) {
        ::getrlimit(RLIMIT_MEMLOCK, &saved_);
        rlimit lowered = saved_;
        lowered.rlim_cur = bytes;
        ::setrlimit(RLIMIT_MEMLOCK, &lowered);
    }
    LockedMemoryLimitLowered(const LockedMemoryLimitLowered&) = delete;
    LockedMemoryLimitLowered& operator=(const LockedMemoryLimitLowered&) = delete;
    LockedMemoryLimitLowered(LockedMemoryLimitLowered&&) = delete;
    LockedMemoryLimitLowered& operator=(LockedMemoryLimitLowered&&) = delete;

  private:
    rlimit saved_{};
};

} // namespaces
