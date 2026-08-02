#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <prometheus/histogram.h>

#include <pubsub_itc_fw/HistogramInterface.hpp>

namespace pubsub_itc_fw {

class PrometheusHistogram : public HistogramInterface {
  public:
    explicit PrometheusHistogram(prometheus::Histogram* histogram) : m_histogram(histogram) {}

    void observe(double value) override {
        m_histogram->Observe(value);
    }

  private:
    prometheus::Histogram* m_histogram;
};

} // namespaces
