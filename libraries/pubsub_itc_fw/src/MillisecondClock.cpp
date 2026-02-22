#include <chrono>
#include <ctime>

#include <pubsub_itc_fw/MillisecondClock.hpp>

namespace pubsub_itc_fw {

MillisecondClock::time_point MillisecondClock::now() noexcept
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);

    // Convert to milliseconds. CLOCK_MONOTONIC_COARSE guarantees millisecond-level resolution and VDSO fast-path access.
    const auto total_ms = static_cast<std::int64_t>(ts.tv_sec) * 1000 + static_cast<std::int64_t>(ts.tv_nsec / 1000000);
    return time_point{duration{total_ms}};
}

} // namespaces
