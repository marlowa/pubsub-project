#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "ArbiterConfiguration.hpp"

namespace arbiter {

/**
 * @brief Loads an ArbiterConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host = "127.0.0.1"
 * listen_port = 7100
 * @endcode
 *
 * All fields are required. Throws ConfigurationException on any error.
 */
class ArbiterConfigurationLoader {
  public:
    /**
     * @param[in] file_path Path to the TOML configuration file.
     * @return Populated ArbiterConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static ArbiterConfiguration load(const std::string& file_path);
};

} // namespace arbiter
