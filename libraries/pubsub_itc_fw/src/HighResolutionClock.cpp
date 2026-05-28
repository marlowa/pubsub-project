// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#ifdef CLANG_TIDY
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <cstdint>
#include <ctime>

#include <pubsub_itc_fw/HighResolutionClock.hpp>

namespace pubsub_itc_fw {

HighResolutionClock::time_point HighResolutionClock::now() {
    struct timespec ts {};
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return time_point(duration(static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec));
}

} // namespace pubsub_itc_fw
