#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include <HistogramInterface.hpp>
#include <pubsub_itc_fw/SingleWriterHistogram.hpp>

namespace pubsub_itc_fw_benchmarks {

/**
 * @brief A SingleWriterHistogram reached through HistogramInterface.
 *
 * Recording through this class does exactly the work a direct call to
 * SingleWriterHistogram::observe does, plus the virtual call, so the difference between the two in
 * the benchmark is the cost of the virtual call and nothing else.
 *
 * Only the source file that creates the histograms may include this header. If the source file
 * holding the benchmark loops could see this class, the compiler would know that it is the only
 * implementation of HistogramInterface and could turn the virtual call into a direct one, and the
 * benchmark would no longer measure a virtual call.
 */
class SingleWriterHistogramThroughInterface : public HistogramInterface {
  public:
    ~SingleWriterHistogramThroughInterface() override = default;

    /** @param[in] upper_bounds Bucket upper bounds, strictly ascending, as SingleWriterHistogram takes. */
    explicit SingleWriterHistogramThroughInterface(const std::vector<double>& upper_bounds) : histogram_(upper_bounds) {}

    SingleWriterHistogramThroughInterface(const SingleWriterHistogramThroughInterface&) = delete;
    SingleWriterHistogramThroughInterface& operator=(const SingleWriterHistogramThroughInterface&) = delete;
    SingleWriterHistogramThroughInterface(SingleWriterHistogramThroughInterface&&) = delete;
    SingleWriterHistogramThroughInterface& operator=(SingleWriterHistogramThroughInterface&&) = delete;

    /**
     * @brief Records one value in the histogram.
     *
     * @param[in] value The value to record.
     */
    void observe(double value) override {
        histogram_.observe(value);
    }

  private:
    pubsub_itc_fw::SingleWriterHistogram histogram_;
};

} // namespaces
