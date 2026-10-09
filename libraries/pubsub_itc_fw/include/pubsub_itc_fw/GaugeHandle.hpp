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
