#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <thread>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

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
    static constexpr uint32_t UP_TO_YIELD = 10;
    static constexpr uint32_t UP_TO_SLEEP = 20;

    /**
     * @brief Performs one step of the backoff sequence.
     */
    void pause() {
#ifdef USING_VALGRIND
        // In Valgrind mode, we skip hardware spinning entirely.
        // Valgrind is serialised; we MUST yield to let other threads run.
        std::this_thread::yield();
#else
        if (count_ < UP_TO_YIELD) {
            // Tier 1: Hardware-level pause (exponentially increasing)
            // On Skylake+, one _mm_pause is ~140 cycles.
            for (uint32_t i = 0; i < (1U << count_); ++i) {
#ifdef __x86_64__
                _mm_pause();
#else
                // Fallback for non-x86 if necessary
                std::this_thread::yield();
#endif
            }
        } else if (count_ < UP_TO_SLEEP) {
            // Tier 2: OS-level yield
            std::this_thread::yield();
        } else {
            // Tier 3: Brief sleep to stop the fan and save power
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        // Prevent overflow while maintaining maximum backoff state
        if (count_ < UP_TO_SLEEP + 1) {
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

} // namespace pubsub_itc_fw
