#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <HistogramInterface.hpp>

namespace pubsub_itc_fw_benchmarks {

/**
 * @brief A copyable handle that records through a HistogramInterface, so every observation is a
 *        virtual call.
 *
 * The same shape as pubsub_itc_fw::HistogramHandle, with one difference: this handle holds a
 * pointer to the interface, so observe() tests the pointer and then makes a virtual call, where
 * HistogramHandle holds a pointer to the concrete histogram and makes no call. A null pointer means
 * metrics are disabled, and observe() then does nothing, in both.
 */
class InterfaceHistogramHandle {
  public:
    InterfaceHistogramHandle() = default;

    /** @param[in] histogram Histogram to record through. Must outlive this handle. */
    explicit InterfaceHistogramHandle(HistogramInterface* histogram) : histogram_(histogram) {}

    /**
     * @brief Records one observation. Does nothing on a default-constructed handle.
     *
     * @param[in] value The value to record.
     */
    void observe(double value) {
        if (histogram_ != nullptr) {
            histogram_->observe(value);
        }
    }

  private:
    HistogramInterface* histogram_ = nullptr;
};

} // namespaces
