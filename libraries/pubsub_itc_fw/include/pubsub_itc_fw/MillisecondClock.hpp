#pragma once

#include <chrono>
#include <ctime>

namespace pubsub_itc_fw {

/**
 * @class MillisecondClock
 *
 * @brief A fast, millisecond-resolution monotonic clock designed specifically for timeout and watchdog logic.
 *
 * This clock provides a stable, monotonic time source with millisecond precision. It is intentionally implemented
 * using CLOCK_MONOTONIC_COARSE, which is guaranteed to be VDSO-accelerated on all modern Linux kernels, including
 * conservative enterprise distributions such as RHEL8.
 *
 * @par Rationale
 * Many standard C++ clocks (such as std::chrono::steady_clock) may fall back to a kernel syscall when VDSO support
 * is unavailable or disabled. On some systems, especially older enterprise kernels or hardened environments, the
 * syscall path introduces measurable overhead, scheduler involvement, and latency jitter. These effects are
 * unacceptable inside tight wait loops used for initialization barriers, inactivity detection, or other
 * high-frequency timeout checks.
 *
 * CLOCK_MONOTONIC_COARSE, however, is always served from the VDSO fast path. This means:
 * - No supervisor call
 * - No context switch
 * - No scheduler involvement
 * - No measurable jitter
 * - Extremely low overhead
 *
 * Although the resolution is coarser (typically 1ms, sometimes 4ms depending on kernel tick rate), this is entirely
 * sufficient for timeout logic where thresholds are measured in milliseconds or seconds. It is not intended for
 * profiling or high-precision measurement.
 *
 * @par Intended Use
 * This clock is designed for:
 * - Initialization timeouts (e.g., threads performing slow startup work)
 * - Runtime inactivity detection (e.g., watchdog logic)
 * - Any timeout-based control flow where millisecond precision is adequate
 *
 * It is not intended for:
 * - Profiling
 * - High-resolution measurement
 * - Sub-millisecond timing
 *
 * @par Example
 * @code
 * const auto start = MillisecondClock::now();
 * while (!condition_met) {
 *     if (MillisecondClock::now() - start > std::chrono::milliseconds(2000)) {
 *         // handle timeout
 *     }
 * }
 * @endcode
 */
class MillisecondClock {
public:
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<MillisecondClock, duration>;

    /**
     * @brief Returns the current monotonic time with millisecond resolution.
     *
     * This function performs a VDSO-backed CLOCK_MONOTONIC_COARSE read, which is extremely fast and does not enter
     * the kernel. The returned time is monotonic and suitable for timeout logic.
     *
     * @return A time_point representing the current time in milliseconds since an unspecified epoch.
     */
    static time_point now() noexcept;
};

} // namespaces
