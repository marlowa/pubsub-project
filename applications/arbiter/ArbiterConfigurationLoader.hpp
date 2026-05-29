#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "ArbiterConfiguration.hpp"

namespace arbiter {

/**
 * @brief Loads ArbiterConfiguration from a TOML file.
 */
class ArbiterConfigurationLoader {
  public:
    /**
     * @brief Load configuration from the given TOML file path.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static ArbiterConfiguration load(const std::string& file_path);
};

} // namespace arbiter
