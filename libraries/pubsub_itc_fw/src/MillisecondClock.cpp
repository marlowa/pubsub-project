// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#ifdef CLANG_TIDY
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <ctime>
#include <cstdint>

#include <pubsub_itc_fw/MillisecondClock.hpp>

namespace pubsub_itc_fw {

MillisecondClock::time_point MillisecondClock::now()
{
    timespec ts{};
    // NOLINTNEXTLINE(misc-include-cleaner)
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);

    // Convert to milliseconds. CLOCK_MONOTONIC_COARSE guarantees millisecond-level resolution and VDSO fast-path access.
    const auto total_ms = static_cast<std::int64_t>(ts.tv_sec) * 1000 + static_cast<std::int64_t>(ts.tv_nsec / 1000000);
    return time_point{duration{total_ms}};
}

} // namespaces
