#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <prometheus/counter.h>

namespace pubsub_itc_fw {

/**
 * @brief A copyable value that records through a counter owned by PrometheusEndpoint.
 *
 * The handle holds a pointer to the prometheus-cpp counter itself, so increment() compiles to a
 * test of the pointer and the counter's own update, with no virtual call and no call through a
 * function pointer in between. A null pointer means metrics are disabled, and increment() then
 * does nothing. A default-constructed handle holds a null pointer, so it records nowhere and is
 * safe to increment.
 *
 * Registration returns one of these rather than a reference for three reasons, all of which
 * bite at the call site rather than here:
 *
 *  - A reference member must be initialised in the constructor's initialiser list, which
 *    runs in member *declaration* order. A metric whose key is built from another member --
 *    a thread name, say -- then depends on the two being declared in the right order, and
 *    getting it wrong captures an empty string silently rather than failing to compile.
 *    A value member can be assigned in the constructor body, and the ordering trap is gone.
 *  - A reference member makes its enclosing class non-assignable. That is a lasting
 *    restriction on a class to have acquired from the decision to count something.
 *  - A default-constructed handle is a safe no-op, so a class can hold one unconditionally
 *    and register only on the paths that turn out to need it.
 *
 * What this does NOT provide is lifetime safety. The pointer dangles if the endpoint is
 * destroyed first, exactly as a reference would. That is not a problem in practice because
 * the endpoint is a Reactor member and outlives everything that registers with it, and
 * because prometheus-cpp keeps each counter in a family that holds it by unique_ptr, so a
 * counter does not move as further counters are registered.
 */
class CounterHandle {
  public:
    CounterHandle() = default;

    /** @param[in] counter Counter to record through. Must outlive this handle. */
    explicit CounterHandle(prometheus::Counter* counter) : counter_(counter) {}

    /** @brief Adds one. Does nothing on a default-constructed handle. */
    void increment() {
        // The test of the pointer costs two machine instructions: a test of the register holding
        // the pointer against itself, and a conditional jump. x86-64 processors fuse the pair into
        // a single operation. The pointer has to be loaded to record through it anyway, so the test
        // adds no access to memory. Whether metrics are enabled is decided once, at start-up, and
        // never changes, so the jump goes the same way on every call and the processor predicts it
        // correctly every time; a correctly predicted jump does not stall the processor.
        //
        // Measured on the histogram handle, which has the same test, recording 200 million values
        // on one pinned core with the test and without it: no difference attributable to the test
        // could be found. Moving where the compiler
        // placed the loop in memory changed the time per call by about a nanosecond, and which of
        // the two versions came out faster depended on that placement, not on the test.
        if (counter_ != nullptr) {
            counter_->Increment();
        }
    }

    /** @brief Whether this handle records anywhere. */
    [[nodiscard]] bool is_bound() const {
        return counter_ != nullptr;
    }

  private:
    prometheus::Counter* counter_ = nullptr;
};

} // namespaces
