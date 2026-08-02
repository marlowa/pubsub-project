#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/HistogramInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A copyable value that records through a histogram owned by PrometheusEndpoint.
 *
 * As CounterHandle, which carries the full rationale for why registration returns a value
 * rather than a reference. This is not a HistogramInterface; it holds a pointer to one.
 */
class HistogramHandle {
  public:
    /** @brief A handle that records nowhere. Observing on it is safe and does nothing. */
    HistogramHandle() = default;

    /** @param[in] histogram Histogram to record through. Must outlive this handle. */
    explicit HistogramHandle(HistogramInterface* histogram) : histogram_(histogram) {}

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
    HistogramInterface* histogram_ = nullptr;
};

} // namespaces
