#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "SequencerConfiguration.hpp"

namespace sequencer {

/**
 * @brief Loads a SequencerConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host = "127.0.0.1"
 * listen_port = 7001
 *
 * [matching_engine]
 * host = "127.0.0.1"
 * port = 7020
 *
 * [ha]
 * instance_id  = 1
 * peer_host    = "127.0.0.1"
 * peer_port    = 7003
 * arbiter_host = "127.0.0.1"
 * arbiter_port = 7100
 * @endcode
 *
 * All fields are required. Throws ConfigurationException on any error.
 */
class SequencerConfigurationLoader {
  public:
    /**
     * @param[in] file_path Path to the TOML configuration file.
     * @return Populated SequencerConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static SequencerConfiguration load(const std::string& file_path);
};

} // namespace sequencer
