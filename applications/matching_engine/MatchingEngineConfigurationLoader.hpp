#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "MatchingEngineConfiguration.hpp"

namespace matching_engine {

/**
 * @brief Loads a MatchingEngineConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host = "127.0.0.1"
 * listen_port = 7020
 *
 * [gateway]
 * host = "127.0.0.1"
 * port = 7010
 * @endcode
 *
 * All fields are required. Throws ConfigurationException on any error.
 */
class MatchingEngineConfigurationLoader {
  public:
    /**
     * @param[in] file_path Path to the TOML configuration file.
     * @return Populated MatchingEngineConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static MatchingEngineConfiguration load(const std::string& file_path);
};

} // namespace matching_engine
