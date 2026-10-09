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
