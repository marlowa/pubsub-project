#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/GaugeInterface.hpp>

namespace pubsub_itc_fw {

class NoOpGauge : public GaugeInterface {
  public:
    void set(double) override {
        // A NoOp implementation does nothing.
    }
};

} // namespaces
