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
