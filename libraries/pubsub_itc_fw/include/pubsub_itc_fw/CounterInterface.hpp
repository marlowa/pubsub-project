#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

class CounterInterface {
  public:
    virtual ~CounterInterface() = default;

    virtual void increment() = 0;
};

} // namespaces
