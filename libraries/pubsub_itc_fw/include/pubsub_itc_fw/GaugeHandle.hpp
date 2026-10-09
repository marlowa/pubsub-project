#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <prometheus/gauge.h>

namespace pubsub_itc_fw {

/**
 * @brief A copyable value that records through a gauge owned by PrometheusEndpoint.
 *
 * As CounterHandle, which carries the full rationale for why registration returns a value
 * rather than a reference. The handle holds a pointer to the prometheus-cpp gauge itself, so
 * set() makes no virtual call. A null pointer means metrics are disabled, and set() then does
 * nothing. A default-constructed handle holds a null pointer, so it records nowhere and is safe
 * to set.
 */
class GaugeHandle {
  public:
    GaugeHandle() = default;

    /** @param[in] gauge Gauge to record through. Must outlive this handle. */
    explicit GaugeHandle(prometheus::Gauge* gauge) : gauge_(gauge) {}

    /** @brief Sets the current value. Does nothing on a default-constructed handle. */
    void set(double value) {
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
        if (gauge_ != nullptr) {
            gauge_->Set(value);
        }
    }

    /** @brief Whether this handle records anywhere. */
    [[nodiscard]] bool is_bound() const {
        return gauge_ != nullptr;
    }

  private:
    prometheus::Gauge* gauge_ = nullptr;
};

} // namespaces
