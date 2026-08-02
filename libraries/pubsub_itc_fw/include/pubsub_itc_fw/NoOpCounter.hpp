#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/CounterInterface.hpp>

namespace pubsub_itc_fw {

class NoOpCounter : public CounterInterface {
  public:
    void increment() override {
        // A NoOp implementation does nothing.
    }
};

} // namespaces
