#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <prometheus/counter.h>

#include <pubsub_itc_fw/CounterInterface.hpp>

namespace pubsub_itc_fw {

class PrometheusCounter : public CounterInterface {
  public:
    explicit PrometheusCounter(prometheus::Counter* counter) : m_counter(counter) {}

    void increment() override {
        m_counter->Increment();
    }

  private:
    prometheus::Counter* m_counter;
};

} // namespaces
