#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <thread>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief A pure spin-wait backoff for nanosecond-scale contention windows.
 *
 * This class provides exponentially increasing spin-wait delays using only
 * hardware-level pause instructions (`_mm_pause` on x86-64). It never calls
 * `std::this_thread::yield()` or `std::this_thread::sleep_for()`.
 *
 * Contrast with BackoffWithYield
 * --------------------------------
 * `BackoffWithYield` is designed for open-ended waiting — for example, a
 * consumer thread spinning on a message queue that may be empty for milliseconds
 * at a time. Its tiered strategy escalates from hardware pauses to OS-level
 * yield to sleep, giving up CPU time when progress is not imminent.
 *
 * `BackoffWithoutYield` is designed for closed, bounded contention windows
 * where the wait is measured in nanoseconds and yielding would be
 * disproportionate and harmful. Specifically:
 *
 *   - The contention window is known to be extremely short (a handful of
 *     CPU cycles). Yielding would introduce a context-switch overhead that
 *     is orders of magnitude larger than the window itself.
 *
 *   - Yielding could cause the very thread we are waiting for to be
 *     descheduled, making the situation worse rather than better.
 *
 *   - The caller has a fixed retry budget and will take a different action
 *     (e.g. fall through to a slow path) if the window does not close within
 *     that budget. Unbounded yielding is therefore inappropriate.
 *
 * Primary use case: lock-free Treiber stack retry in FixedSizeMemoryPool
 * -----------------------------------------------------------------------
 * `push_slot_to_free_list()` writes `slot->free_next` (step A) and then
 * updates `head_raw_` via CAS (step B). Between A and B the slot is not yet
 * reachable from the free-list head. A concurrent `pop_slot_from_free_list()`
 * that samples the head between A and B sees the list as transiently empty
 * and would spuriously return `nullptr`, triggering unnecessary pool expansion.
 *
 * `BackoffWithoutYield` is used in the pop retry loop to wait out this
 * window. The window closes in a handful of cycles — a few exponentially
 * increasing `_mm_pause` calls is all that is needed. Using `BackoffWithYield`
 * here would cause unnecessary fan noise and CPU heat under sustained stress
 * because its tier-2 yield and tier-3 sleep are triggered far too aggressively
 * for a nanosecond-scale window.
 *
 * Behaviour
 * ---------
 * Each call to `pause()` emits an exponentially increasing number of
 * `_mm_pause` instructions: 1, 2, 4, 8, ... up to a ceiling of
 * `max_pauses_per_step`. The counter never resets automatically — call
 * `reset()` when progress is made.
 *
 * On non-x86 platforms `_mm_pause` is not available. A compiler barrier
 * (`__asm__ volatile("" ::: "memory")`) is used instead, which prevents
 * the loop from being optimised away while adding no hardware delay. On
 * such platforms `BackoffWithoutYield` degenerates to a simple retry loop.
 *
 * Under USING_VALGRIND (Valgrind or TSan builds), the implementation
 * falls back to a single `std::this_thread::yield()` per step — the same
 * as `BackoffWithYield` tier 2 — because those tools cannot model the
 * hardware pause instruction and need standard synchronisation primitives
 * to reason correctly about thread ordering.
 *
 * Thread safety
 * -------------
 * Each `BackoffWithoutYield` instance is intended to be used by a single
 * thread. No internal state is shared between threads.
 *
 * Example usage
 * -------------
 * @code
 * BackoffWithoutYield backoff;
 * for (int attempt = 0; attempt < max_retries; ++attempt) {
 *     if (try_operation()) return success;
 *     backoff.pause();
 * }
 * // fall through to slow path
 * @endcode
 */
class BackoffWithoutYield {
public:
    /**
     * @brief Maximum number of `_mm_pause` calls emitted in a single `pause()` step.
     *
     * Caps the exponential growth to prevent a single pause() call from
     * consuming an excessive number of cycles. At 256 pauses × ~140 cycles
     * each (Skylake+), the ceiling is approximately 35,000 cycles (~12 µs at
     * 3 GHz) — still short enough to be appropriate for contention windows
     * measured in nanoseconds, while giving concurrent threads ample time to
     * complete a CAS.
     */
    static constexpr uint32_t max_pauses_per_step = 256;

    /**
     * @brief Performs one step of the spin-wait sequence.
     *
     * Emits an exponentially increasing number of hardware pause instructions.
     * The count doubles with each call until `max_pauses_per_step` is reached,
     * after which it remains constant.
     *
     * Never yields to the OS scheduler and never sleeps. If you need those
     * behaviours, use `BackoffWithYield` instead.
     */
    void pause() {
#ifdef USING_VALGRIND
        // Valgrind and TSan cannot model _mm_pause. Fall back to a yield so
        // these tools can reason correctly about thread ordering.
        std::this_thread::yield();
#else
        const uint32_t pauses = 1U << count_;
        for (uint32_t i = 0; i < pauses; ++i) {
#ifdef __x86_64__
            _mm_pause();
#else
            // No hardware pause available. A compiler barrier prevents the
            // loop from being optimised away.
            __asm__ volatile("" ::: "memory");
#endif
        }

        // Advance the counter, clamping at the step that produces max_pauses_per_step.
        if ((1U << count_) < max_pauses_per_step) {
            ++count_;
        }
#endif
    }

    /**
     * @brief Resets the backoff counter.
     *
     * Call this when progress is made so that the next contention window
     * starts from the minimum pause duration.
     */
    void reset() {
        count_ = 0;
    }

private:
    uint32_t count_{0};
};

} // namespace pubsub_itc_fw
