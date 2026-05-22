#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

/*
HighResolutionClock
-------------------

Nanosecond-resolution monotonic clock used for SLA-critical timers.

Why not reuse MillisecondClock?
--------------------------------
MillisecondClock is intentionally coarse and VDSO-accelerated. It is ideal for
lifecycle timeouts and backoff loops, but NOT suitable for microsecond-level
timers. Users of this framework may require timers accurate to a few
milliseconds or better.

Why CLOCK_MONOTONIC?
---------------------
Stable, never goes backwards, unaffected by wall-clock adjustments, and is the
basis for kernel hrtimers (used by timerfd). This avoids leap seconds and
timezone issues.

RHEL8 + GCC 8.5 note:
----------------------
Some older glibc builds may fall back to a syscall for
clock_gettime(CLOCK_MONOTONIC). This is acceptable because this clock is used
only for *reading* time (e.g., computing durations). Actual timer scheduling is
performed by timerfd, which uses kernel hrtimers and is unaffected by VDSO
behaviour.
*/

namespace pubsub_itc_fw {

class HighResolutionClock {
  public:
    using duration = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<HighResolutionClock, duration>;

    static time_point now();
};

} // namespace pubsub_itc_fw
