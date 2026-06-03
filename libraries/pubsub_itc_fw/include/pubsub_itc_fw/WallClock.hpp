#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>

namespace pubsub_itc_fw {

/** @ingroup instrumentation_subsystem */

/**
 * @brief Abstract interface for wall-clock time acquisition.
 *
 * All components that need the current wall time acquire it through this
 * interface rather than calling `std::chrono::system_clock::now()` directly.
 * This makes the time source injectable, enabling:
 *
 * - **Unit testing**: supply a deterministic mock clock.
 * - **Replay**: supply a ReplayClock driven by the sequencer's WAL timestamps
 *   so that every component downstream of the sequencer sees the original
 *   event time rather than the wall time at which the replay is running.
 *
 * @see SystemWallClock for the production implementation.
 * @see ReplayClock for the replay and test implementation.
 */
class WallClock {
  public:
    virtual ~WallClock() = default;

    /**
     * @brief Returns the current time as nanoseconds since the Unix epoch (UTC).
     */
    [[nodiscard]] virtual int64_t now_ns() const = 0;
};

/**
 * @brief WallClock implementation backed by `std::chrono::system_clock`.
 *
 * Used in normal production operation. Returns the true UTC wall time.
 *
 * @ingroup instrumentation_subsystem
 */
class SystemWallClock final : public WallClock {
  public:
    [[nodiscard]] int64_t now_ns() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

/**
 * @brief WallClock implementation with a settable, thread-safe time value.
 *
 * The current time is stored as an `atomic<int64_t>` and can be advanced by
 * calling `set_time_ns()`. Intended for two use cases:
 *
 * 1. **Replay**: the sequencer advances the clock to the `sequenced_at`
 *    timestamp read from each WAL record before dispatching that record to
 *    downstream components.
 *
 * 2. **Unit testing**: set an exact, reproducible time in test fixtures.
 *
 * @ingroup instrumentation_subsystem
 */
class ReplayClock final : public WallClock {
  public:
    /**
     * @param[in] initial_ns Starting time in nanoseconds since the Unix epoch.
     */
    explicit ReplayClock(int64_t initial_ns = 0) : time_ns_(initial_ns) {}

    [[nodiscard]] int64_t now_ns() const override {
        return time_ns_.load(std::memory_order_acquire);
    }

    /**
     * @brief Advances the clock to the given time.
     *
     * Thread-safe. Typically called by the sequencer replay loop before
     * dispatching each WAL record to downstream components.
     *
     * @param[in] time_ns New time in nanoseconds since the Unix epoch.
     */
    void set_time_ns(int64_t time_ns) {
        time_ns_.store(time_ns, std::memory_order_release);
    }

  private:
    std::atomic<int64_t> time_ns_;
};

} // namespace pubsub_itc_fw
