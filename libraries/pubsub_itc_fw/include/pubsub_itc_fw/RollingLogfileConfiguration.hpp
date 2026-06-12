#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <stdexcept>
#include <string>

namespace pubsub_itc_fw {

struct RollingLogfileConfiguration {
    enum class Mode { None, Size, Daily };
    Mode mode = Mode::None;

    // Size-based params
    int64_t max_file_size{50 * 1024 * 1024}; // 50MB default

    int32_t max_backup_files{10}; // Number of rolled files to keep

    // Time-based params
    std::string rotation_time{"00:00"}; // Daily rotation time, "HH:MM" format

    RollingLogfileConfiguration() = default;
    explicit RollingLogfileConfiguration(Mode mode_arg) : mode(mode_arg) {}

    static RollingLogfileConfiguration daily(const std::string& time_hh_mm) {
        if (time_hh_mm.size() != 5 || time_hh_mm[2] != ':') {
            throw std::invalid_argument("rotation_time must be in HH:MM format, got: " + time_hh_mm);
        }
        const int hours = std::stoi(time_hh_mm.substr(0, 2));
        const int minutes = std::stoi(time_hh_mm.substr(3, 2));
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
            throw std::invalid_argument("rotation_time out of range: " + time_hh_mm);
        }
        RollingLogfileConfiguration config{Mode::Daily};
        config.rotation_time = time_hh_mm;
        return config;
    }
};

} // namespaces
