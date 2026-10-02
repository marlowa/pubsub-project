#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <string>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "ThrottleLimits.hpp"

namespace fix_common {

/**
 * @brief Allows at most a given number of commands in any one-second period.
 *
 * A command is accepted if fewer than the limit were accepted in the second before it arrived.
 * The second is measured back from the moment the command arrives, not from the start of a clock
 * second, so however a one-second period is placed, it never holds more accepted commands than
 * the limit. This is the rule in docs/venue/gateway_throttles.md, section 2.
 *
 * **How it works.** The throttle holds the arrival times of the commands it accepted, in a ring
 * buffer with room for exactly as many times as the limit. Asked about a command arriving at time
 * T, it first removes from the front of the buffer every time that is one second or more before T.
 * Times are added in order, so the old ones are always at the front. It then tries to add T. If the
 * buffer is full, the limit was reached in the last second, and the command is refused; otherwise
 * T is recorded and the command accepted. Each time is added once and removed once, so the cost of
 * a decision does not depend on the limit.
 *
 * Three details of the rule:
 * - A command arriving exactly one second after the oldest time held is accepted: that time has
 *   then left the window.
 * - A refused command is not recorded, so it does not count towards the limit. Otherwise a member
 *   that kept retrying while refused would keep itself refused indefinitely.
 * - The caller gives the time, rather than the throttle reading a clock, so that the caller reads
 *   the clock once for each command and a test can say exactly when each command arrives. The time
 *   must come from std::chrono::steady_clock, which never moves backwards: a correction to the
 *   system clock would make the window count wrongly.
 *
 * **Memory.** All the storage the throttle will ever use is taken when it is created, sized for
 * exactly its limit: eight bytes for each command it allows in a second. Deciding about a command
 * never uses the heap, and the throttle never grows. A full buffer does not need to grow, because a
 * full buffer is the throttle refusing the command.
 *
 * **Threading.** Not thread-safe. A gateway handles each session on one thread.
 */
class RollingWindowThrottle {
  public:
    using Clock = std::chrono::steady_clock;

    /// The period the limit applies to.
    static constexpr Clock::duration window = std::chrono::seconds(1);

    ~RollingWindowThrottle() = default;

    /**
     * @brief Creates a throttle that accepts at most @p max_per_second commands in any one second.
     *
     * A limit of zero means no limit, and is represented by having no throttle at all, so zero is
     * refused here.
     *
     * @param[in] max_per_second   The limit, from 1 to ThrottleLimits::max_permitted_per_second.
     * @param[in] growth_reporter  Non-owning, may be nullptr. Told the size of the storage taken.
     */
    explicit RollingWindowThrottle(int max_per_second, pubsub_itc_fw::AllocationGrowthReporter* growth_reporter = nullptr)
        : accepted_times_(validated_capacity(max_per_second), growth_reporter), max_per_second_(max_per_second) {}

    RollingWindowThrottle(const RollingWindowThrottle& other) = delete;
    RollingWindowThrottle& operator=(const RollingWindowThrottle& other) = delete;
    RollingWindowThrottle(RollingWindowThrottle&& other) = default;
    RollingWindowThrottle& operator=(RollingWindowThrottle&& other) = default;

    /**
     * @brief Decides whether a command arriving at @p now is within the limit, and if so counts it.
     *
     * @param[in] now When the command arrived, from std::chrono::steady_clock. Never earlier than
     *                the time given to the previous call.
     * @return True if the command is within the limit, in which case it counts towards the limit
     *         for the next second. False if the limit has been reached, in which case nothing is
     *         recorded and the caller must refuse the command.
     */
    [[nodiscard]] bool try_accept(Clock::time_point now) {
        while (!accepted_times_.empty() && now - accepted_times_.front() >= window) {
            accepted_times_.pop_front();
        }
        return accepted_times_.push_back(now);
    }

    /// The largest number of commands accepted in any one second.
    [[nodiscard]] int max_per_second() const {
        return max_per_second_;
    }

    /// The number of accepted commands currently held. Times that have left the window are only
    /// removed when a command arrives, so this can include some that no longer count.
    [[nodiscard]] size_t recorded_count() const {
        return accepted_times_.size();
    }

    /// The bytes of storage the throttle took when it was created.
    [[nodiscard]] size_t allocated_bytes() const {
        return accepted_times_.allocated_bytes();
    }

  private:
    /// Checks the limit before the ring buffer is created, so that the error names the throttle's
    /// own rule rather than the buffer's.
    [[nodiscard]] static size_t validated_capacity(int max_per_second) {
        if (max_per_second < 1 || max_per_second > ThrottleLimits::max_permitted_per_second) {
            throw pubsub_itc_fw::PreconditionAssertion("RollingWindowThrottle: the limit must be from 1 to " +
                                                           std::to_string(ThrottleLimits::max_permitted_per_second) + ", but was " +
                                                           std::to_string(max_per_second),
                                                       __FILE__, __LINE__);
        }
        return static_cast<size_t>(max_per_second);
    }

    pubsub_itc_fw::FixedCapacityRingBuffer<Clock::time_point> accepted_times_;
    int max_per_second_;
};

} // namespaces
