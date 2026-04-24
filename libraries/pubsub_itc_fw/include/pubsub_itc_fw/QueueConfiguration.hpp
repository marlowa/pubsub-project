#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <functional>

namespace pubsub_itc_fw {

class QueueConfiguration {
  public:
    // Watermark thresholds
    int low_watermark{0};
    int high_watermark{0};

    // Passed back to the client’s watermark handlers
    void* for_client_use{nullptr};

    // Called when queue size drops below low_watermark
    std::function<void(void* for_client_use)> gone_below_low_watermark_handler;

    // Called when queue size rises above high_watermark
    std::function<void(void* for_client_use)> gone_above_high_watermark_handler;

    QueueConfiguration() = default;
};

} // namespace pubsub_itc_fw
