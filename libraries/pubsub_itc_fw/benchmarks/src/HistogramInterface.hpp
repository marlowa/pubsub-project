#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw_benchmarks {

/**
 * @brief An abstract histogram, recorded into through a virtual function.
 *
 * The benchmark's own copy of the class hierarchy a metrics library would normally offer: an
 * interface, with one implementation for each kind of histogram. Recording through it costs a
 * virtual call, which is what the benchmark measures against a handle that holds the concrete
 * histogram type and makes no call at all.
 */
class HistogramInterface {
  public:
    virtual ~HistogramInterface() = default;

    /**
     * @brief Records one value.
     *
     * @param[in] value The value to record.
     */
    virtual void observe(double value) = 0;
};

} // namespaces
