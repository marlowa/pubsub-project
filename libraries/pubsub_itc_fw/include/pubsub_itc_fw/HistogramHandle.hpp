#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/SingleWriterHistogram.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A copyable value that records through a histogram owned by PrometheusEndpoint.
 *
 * As CounterHandle, which carries the full rationale for why registration returns a value
 * rather than a reference. The handle holds a pointer to the SingleWriterHistogram itself, so
 * observe() compiles into the caller: a test of the pointer, the search of the bucket bounds
 * and two stores, with no call at all. A null pointer means metrics are disabled, and observe()
 * then does nothing. A default-constructed handle holds a null pointer, so it records nowhere and
 * is safe to observe on.
 */
class HistogramHandle {
  public:
    HistogramHandle() = default;

    /** @param[in] histogram Histogram to record through. Must outlive this handle. */
    explicit HistogramHandle(SingleWriterHistogram* histogram) : histogram_(histogram) {}

    /** @brief Records one observation. Does nothing on a default-constructed handle. */
    void observe(double value) {
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
        if (histogram_ != nullptr) {
            histogram_->observe(value);
        }
    }

    /** @brief Whether this handle records anywhere. */
    [[nodiscard]] bool is_bound() const {
        return histogram_ != nullptr;
    }

  private:
    SingleWriterHistogram* histogram_ = nullptr;
};

} // namespaces
