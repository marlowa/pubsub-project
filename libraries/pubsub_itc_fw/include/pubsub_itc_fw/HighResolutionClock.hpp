#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

namespace pubsub_itc_fw {

/** @ingroup instrumentation_subsystem */

/**
 * @brief Nanosecond-resolution monotonic clock for SLA-critical timing and latency measurement.
 *
 * Uses `CLOCK_MONOTONIC` via `clock_gettime()`. The clock is stable, never goes backwards,
 * and is unaffected by wall-clock adjustments or leap seconds. It is the same time source
 * as kernel hrtimers (`timerfd`).
 *
 * @par Why not MillisecondClock?
 * MillisecondClock uses `CLOCK_MONOTONIC_COARSE` and is always VDSO-accelerated, making it
 * ideal for lifecycle timeouts and backoff loops. It is intentionally coarse and not suitable
 * for microsecond-level measurement. Use HighResolutionClock when nanosecond precision is
 * required (e.g., latency recording, SLA enforcement, profiling).
 *
 * @par RHEL8 / older glibc note
 * On some older glibc builds `clock_gettime(CLOCK_MONOTONIC)` may not be VDSO-accelerated
 * and can fall back to a syscall. This is acceptable because this clock is used only for
 * reading time to compute durations, not for scheduling; actual timer delivery is handled
 * by `timerfd` and is unaffected.
 *
 * @see MillisecondClock for coarse, always-VDSO-accelerated timeout logic.
 */
class HighResolutionClock {
  public:
    using duration   = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<HighResolutionClock, duration>;

    /**
     * @brief Returns the current monotonic time with nanosecond resolution.
     * @return A time_point representing the current time in nanoseconds since an unspecified epoch.
     */
    static time_point now();
};

} // namespaces
