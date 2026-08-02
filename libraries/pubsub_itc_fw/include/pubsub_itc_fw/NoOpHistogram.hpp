#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/HistogramInterface.hpp>

namespace pubsub_itc_fw {

class NoOpHistogram : public HistogramInterface {
  public:
    void observe(double) override {
        // A NoOp implementation does nothing.
    }
};

} // namespaces
