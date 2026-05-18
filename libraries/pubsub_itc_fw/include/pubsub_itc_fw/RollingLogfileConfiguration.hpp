#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <string>

namespace pubsub_itc_fw {

struct RollingLogfileConfiguration {
    enum class Mode { None, Size, Daily };
    Mode mode = Mode::None;

    // Size-based params
    int64_t max_file_size{50 * 1024 * 1024}; // 50MB default

    int32_t max_backup_files{10}; // Number of rolled files to keep

    // Time-based params
    std::string rotation_time{"00:00"}; // Daily rotation time

    // TODO Add ctor that validates the rotation time
};

} // namespace pubsub_itc_fw
