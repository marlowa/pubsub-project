#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "BinaryOrderGatewayConfiguration.hpp"

namespace binary_order_gateway {

/**
 * @brief Loads a BinaryOrderGatewayConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host    = "127.0.0.1"
 * listen_port    = 9890
 * er_listen_host = "127.0.0.1"
 * er_listen_port = 7110
 *
 * [sequencer]
 * ha_enabled     = true
 * primary_host   = "127.0.0.1"
 * primary_port   = 7001
 * secondary_host = "127.0.0.1"
 * secondary_port = 7002
 * @endcode
 *
 * Plus the [logging], [reactor], [event_queue_pool] and [command_queue_pool]
 * sections common to every application. All fields are required; the secondary
 * sequencer only when ha_enabled is true. Throws ConfigurationException on any error.
 */
class BinaryOrderGatewayConfigurationLoader {
  public:
    /**
     * @brief Loads and validates configuration from the given TOML file path.
     *
     * Also starts the logger, since the rolling-file settings it needs live in the
     * same file and are wanted before anything else can report a problem.
     *
     * @param[in] file_path Path to the TOML configuration file.
     * @param[in] log_file_path Pathname for the application logfile.
     * @return Populated BinaryOrderGatewayConfiguration and the created logger.
     */
    static std::tuple<BinaryOrderGatewayConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>> load_and_init_logging(const std::string& file_path,
                                                                                                                          const std::string& log_file_path);
};

} // namespaces
