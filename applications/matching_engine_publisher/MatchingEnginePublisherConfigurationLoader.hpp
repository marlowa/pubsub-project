#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "MatchingEnginePublisherConfiguration.hpp"

namespace matching_engine_publisher {

/**
 * @brief Loads MatchingEnginePublisherConfiguration from a TOML file.
 *
 * Throws pubsub_itc_fw::ConfigurationException on any error.
 */
class MatchingEnginePublisherConfigurationLoader {
  public:
    static MatchingEnginePublisherConfiguration load(const std::string& file_path);
};

} // namespaces
