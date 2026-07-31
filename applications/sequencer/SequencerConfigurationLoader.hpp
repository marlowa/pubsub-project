#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/TomlConfiguration.hpp>

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

    /**
     * @brief Loads from an already-parsed TOML document.
     *
     * Split from the file-path overload so the parsing rules can be tested without a
     * filesystem fixture -- the same seam LoggingConfigurationLoader offers. The file
     * overload reads the document and delegates here.
     *
     * @param[in] toml Parsed configuration document.
     * @return Populated SequencerConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static SequencerConfiguration load(const pubsub_itc_fw::TomlConfiguration& toml);
};

} // namespaces
