#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "WitnessConfiguration.hpp"

namespace witness {

/**
 * @brief Loads a WitnessConfiguration from a TOML file.
 *
 * All fields are required. Throws ConfigurationException on any error.
 */
class WitnessConfigurationLoader {
  public:
    /**
     * @param[in] file_path Path to the TOML configuration file.
     * @return Populated WitnessConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static WitnessConfiguration load(const std::string& file_path);
};

} // namespaces
