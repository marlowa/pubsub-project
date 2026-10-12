#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ArbiterConfiguration.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Loads ArbiterConfiguration from a TOML file.
 */
class ArbiterConfigurationLoader {
  public:
    /**
     * @brief Load configuration from the given TOML file path.
     * Throws ConfigurationException on any error.
     */
    static ArbiterConfiguration load(const std::string& file_path);

    /**
     * @brief Load configuration already read into @p toml.
     * Throws ConfigurationException on any error.
     */
    static ArbiterConfiguration load(const pubsub_itc_fw::TomlConfiguration& toml);
};

} // namespaces
