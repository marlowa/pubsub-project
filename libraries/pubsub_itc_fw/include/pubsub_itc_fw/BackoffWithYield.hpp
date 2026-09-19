#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <thread>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief The processor's spin-wait hint: one iteration of waiting that does no work.
 *
 * Tells the processor that the surrounding loop is waiting rather than computing. Without the
 * hint the loop issues speculative loads which are all invalidated the moment the awaited value
 * changes, and the processor pays a pipeline flush at exactly the instant the work arrives --
 * the instant the loop exists to make fast. It also hands execution resources back to the other
 * hardware thread sharing the core, and draws less power.
 *
 * This is the primitive BackoffWithYield uses for its first tier. It is exposed separately for
 * the caller that wants only that tier: a bounded wait which must not yield or sleep, because
 * yielding and sleeping are context switches and avoiding those is the whole point. Anything
 * waiting for an unbounded time should use BackoffWithYield instead, so that it eventually
 * stands aside rather than holding a core indefinitely.
 */
inline void cpu_relax() {
#ifdef __x86_64__
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

/**
 * @brief Utility for exponential backoff in spin-loops.
 * * Provides a tiered strategy:
 * 1. Hardware-hinted spinning (PAUSE) for extremely short waits.
 * 2. Thread yielding to allow other threads (or Valgrind) to progress.
 * 3. Targeted sleeping for sustained contention to reduce CPU heat/noise.
 */
class BackoffWithYield {
  public:
    // Constants for tuning the backoff behaviour
    static constexpr uint32_t up_to_yield = 10;
    static constexpr uint32_t up_to_sleep = 20;

    /**
     * @brief Performs one step of the backoff sequence.
     */
    void pause() {
#ifdef USING_VALGRIND
        // In Valgrind mode, we skip hardware spinning entirely.
        // Valgrind is serialised; we MUST yield to let other threads run.
        std::this_thread::yield();
#else
        if (count_ < up_to_yield) {
            // Tier 1: Hardware-level pause (exponentially increasing)
            // On Skylake+, one _mm_pause is ~140 cycles.
            for (uint32_t i = 0; i < (1U << count_); ++i) {
                cpu_relax();
            }
        } else if (count_ < up_to_sleep) {
            // Tier 2: OS-level yield
            std::this_thread::yield();
        } else {
            // Tier 3: Brief sleep to stop the fan and save power
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        // Prevent overflow while maintaining maximum backoff state
        if (count_ < up_to_sleep + 1) {
            count_++;
        }
#endif
    }

    /**
     * @brief Resets the backoff counter.
     * Call this whenever progress is made (e.g., a message is successfully dequeued).
     */
    void reset() {
        count_ = 0;
    }

  private:
    uint32_t count_ = 0;
};

} // namespaces
