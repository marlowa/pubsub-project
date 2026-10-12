#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Refuses to let a second thread touch state that one thread owns.
 *
 * Principle F1 in docs/design_principles.md: each piece of changing state has exactly one owning
 * thread. A class whose state is owned by one thread holds one of these and calls check() at the
 * start of every operation that reads or changes that state. The first thread to call check()
 * becomes the owner; any other thread that calls it afterwards is refused with a
 * PreconditionAssertion naming what it touched.
 *
 * Reads are checked as well as writes, because a reader on the wrong thread races a writer just as
 * a second writer would.
 *
 * The check costs a read of the calling thread's id on every operation, which the order path should
 * not pay to verify something the design already guarantees. So a class holds one only in builds
 * with PUBSUB_ITC_FW_THREAD_CHECKS defined, which the top-level CMakeLists sets for Debug,
 * AddressSanitizer and coverage builds, and the release build has neither the member nor the call.
 *
 * **A known breach is counted, not refused.** Where a second thread is already known to touch the
 * state, and a bug records it with the phase of docs/recovery_plan.md that removes it, the code that
 * owns the state calls mark_known_breach() with that bug and phase, before the state is first used.
 * check() then lets the second thread through and counts each time it does, so that the program goes
 * on running and the breach can be seen happening. The owner of the state reads the count with
 * known_breach_count(), logs one warning when it first becomes non-zero, and publishes it as a
 * metric. When the breach is removed, the count stays at zero, and the call to mark_known_breach()
 * is deleted, so that a second thread is refused again.
 *
 * The owner is held in an atomic variable because, under a known breach, two threads do call
 * check() at the same time.
 */
class ThreadConfinementCheck {
  public:
    /**
     * @param[in] what_is_guarded Named in the refusal, for example "IncrementalRehashMap". Must
     *            outlive this object; a string literal does.
     */
    explicit ThreadConfinementCheck(const char* what_is_guarded) : what_is_guarded_(what_is_guarded) {}

    ThreadConfinementCheck(const ThreadConfinementCheck&) = delete;
    ThreadConfinementCheck& operator=(const ThreadConfinementCheck&) = delete;

    /**
     * @brief Says that a second thread is known to touch this state, and that it is to be counted
     *        rather than refused.
     *
     * @param[in] bug_and_phase The bug that records the breach and the phase that removes it, for
     *            example "BUG-0124, removed in phase 4". Must outlive this object; a string
     *            literal does.
     *
     * Called by the code that owns the state, before the state is first used.
     */
    void mark_known_breach(const char* bug_and_phase) {
        known_breach_ = bug_and_phase;
    }

    /**
     * @return The bug and phase given to mark_known_breach(), or nullptr if there is none.
     */
    [[nodiscard]] const char* known_breach() const {
        return known_breach_;
    }

    /**
     * @return How many times a second thread has been let through under a known breach.
     */
    [[nodiscard]] int64_t known_breach_count() const {
        return known_breach_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Leaves the state with no owner, so that the next thread to call check() becomes its
     *        owner.
     *
     * For state that one thread prepares and then hands to another, once, before that other thread
     * starts using it: for example, a reactor's tables, filled by the thread that builds the reactor
     * and then used by the thread that runs it. Called by the thread taking over, before it uses the
     * state, and only while no other thread is using it.
     */
    void hand_over() {
        owning_thread_.store(std::thread::id{}, std::memory_order_release);
    }

    /**
     * @brief Records the calling thread as the owner if there is none yet, and refuses any other
     *        thread, or counts it under a known breach.
     *
     * const, with the owner held mutable, because a lookup establishes ownership just as a change
     * does.
     */
    void check() const {
        const std::thread::id calling_thread = std::this_thread::get_id();
        std::thread::id owner = owning_thread_.load(std::memory_order_acquire);
        if (owner == std::thread::id{}) {
            if (owning_thread_.compare_exchange_strong(owner, calling_thread, std::memory_order_acq_rel)) {
                return;
            }
            // Another thread became the owner first; owner now holds its id.
        }
        if (owner == calling_thread) {
            return;
        }
        if (known_breach_ != nullptr) {
            known_breach_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        throw PreconditionAssertion(std::string(what_is_guarded_) + " is owned by one thread and was touched by a second thread", __FILE__, __LINE__);
    }

  private:
    const char* what_is_guarded_;
    const char* known_breach_{nullptr};
    mutable std::atomic<std::thread::id> owning_thread_{};
    mutable std::atomic<int64_t> known_breach_count_{0};
};

} // namespaces
