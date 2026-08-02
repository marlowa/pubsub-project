#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/GaugeInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A copyable value that records through a gauge owned by PrometheusEndpoint.
 *
 * As CounterHandle, which carries the full rationale for why registration returns a value
 * rather than a reference. This is not a GaugeInterface; it holds a pointer to one.
 */
class GaugeHandle {
  public:
    /** @brief A handle that records nowhere. Setting it is safe and does nothing. */
    GaugeHandle() = default;

    /** @param[in] gauge Gauge to record through. Must outlive this handle. */
    explicit GaugeHandle(GaugeInterface* gauge) : gauge_(gauge) {}

    /** @brief Sets the current value. Does nothing on a default-constructed handle. */
    void set(double value) {
        if (gauge_ != nullptr) {
            gauge_->set(value);
        }
    }

    /** @brief Whether this handle records anywhere. */
    [[nodiscard]] bool is_bound() const {
        return gauge_ != nullptr;
    }

  private:
    GaugeInterface* gauge_ = nullptr;
};

} // namespaces
