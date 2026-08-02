#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

class HistogramInterface {
  public:
    virtual ~HistogramInterface() = default;

    virtual void observe(double value) = 0;
};

} // namespaces
