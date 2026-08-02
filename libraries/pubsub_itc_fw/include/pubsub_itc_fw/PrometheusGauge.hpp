#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <prometheus/histogram.h>

#include <pubsub_itc_fw/GaugeInterface.hpp>

namespace pubsub_itc_fw {

class PrometheusGauge : public GaugeInterface {
  public:
    explicit PrometheusGauge(prometheus::Gauge* gauge) : m_gauge(gauge) {}

    void set(double value) override {
        m_gauge->Set(value);
    }

  private:
    prometheus::Gauge* m_gauge;
};

} // namespaces
