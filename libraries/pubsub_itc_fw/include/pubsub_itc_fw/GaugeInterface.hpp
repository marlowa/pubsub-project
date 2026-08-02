#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

class GaugeInterface {
  public:
    virtual ~GaugeInterface() = default;

    virtual void set(double value) = 0;
};

} // namespaces
