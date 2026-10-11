#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

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
 * One thread calls check() first and that thread alone thereafter, so the recorded owner needs no
 * synchronisation in correct use. Its purpose is to make incorrect use visible.
 */
class ThreadConfinementCheck {
  public:
    /**
     * @param[in] what_is_guarded Named in the refusal, for example "IncrementalRehashMap". Must
     *            outlive this object; a string literal does.
     */
    explicit ThreadConfinementCheck(const char* what_is_guarded) : what_is_guarded_(what_is_guarded) {}

    /**
     * @brief Records the calling thread as the owner if there is none yet, and refuses any other
     *        thread.
     *
     * const, with the owner held mutable, because a lookup establishes ownership just as a change
     * does.
     */
    void check() const {
        const std::thread::id calling_thread = std::this_thread::get_id();
        if (owning_thread_ == std::thread::id{}) {
            owning_thread_ = calling_thread;
            return;
        }
        if (owning_thread_ != calling_thread) {
            throw PreconditionAssertion(std::string(what_is_guarded_) + " is owned by one thread and was touched by a second thread", __FILE__, __LINE__);
        }
    }

  private:
    const char* what_is_guarded_;
    mutable std::thread::id owning_thread_{};
};

} // namespaces
