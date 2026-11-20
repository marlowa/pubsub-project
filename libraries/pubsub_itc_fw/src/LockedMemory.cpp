// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/LockedMemory.hpp>

#include <cerrno>
#include <cstring>
#include <string>

#include <sys/resource.h>
#include <unistd.h>

namespace pubsub_itc_fw {

LockedMemoryLimit LockedMemoryLimit::current() {
    LockedMemoryLimit limit;
    rlimit read{};
    limit.known_ = ::getrlimit(RLIMIT_MEMLOCK, &read) == 0;
    limit.unlimited_ = limit.known_ && read.rlim_cur == RLIM_INFINITY;
    limit.bytes_ = limit.known_ && !limit.unlimited_ ? static_cast<uint64_t>(read.rlim_cur) : 0;
    limit.runs_as_root_ = ::geteuid() == 0;
    return limit;
}

bool LockedMemoryLimit::allows(size_t bytes) const {
    return !known_ || unlimited_ || runs_as_root_ || bytes <= bytes_;
}

std::string LockedMemoryLimit::text() const {
    if (!known_) {
        return "unknown";
    }
    if (unlimited_) {
        return "unlimited";
    }
    return std::to_string(bytes_ / bytes_in_a_megabyte) + " MB";
}

std::string why_locking_failed(int error) {
    switch (error) {
        case ENOMEM:
            return "the process's limit on locked memory is too low, or the machine does not have the memory (ENOMEM)";
        case EAGAIN:
            return "some of the memory could not be locked, because the machine does not have it free (EAGAIN)";
        case EPERM:
            return "the process may not lock memory: its limit on locked memory is zero (EPERM)";
        default:
            return std::string(std::strerror(error));
    }
}

} // namespaces
