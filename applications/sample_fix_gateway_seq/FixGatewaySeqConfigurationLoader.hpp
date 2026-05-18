#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include "FixGatewaySeqConfiguration.hpp"

#include <pubsub_itc_fw/QuillLogger.hpp>

namespace sample_fix_gateway_seq {

/**
 * @brief Loads a FixGatewaySeqConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host         = "127.0.0.1"
 * listen_port         = 9879
 * raw_buffer_capacity = 65536
 * er_listen_host      = "127.0.0.1"
 * er_listen_port      = 7010
 *
 * [sequencer]
 * primary_host   = "127.0.0.1"
 * primary_port   = 7001
 *
 * [fix_session]
 * sender_comp_id         = "GATEWAY"
 * default_target_comp_id = "CLIENT"
 *
 * [timeouts]
 * logon_timeout = "30s"
 * @endcode
 *
 * All fields are required. Throws ConfigurationException on any error.
 */
class FixGatewaySeqConfigurationLoader {
  public:
    /**
     * @brief Loads and validates configuration from the given TOML file path.
     * note: this includes getting the logger going with any config for rolling on size or time.
     *
     * @param[in] file_path Path to the TOML configuration file.
     * @param[in] log_file_path the pathname for the application logfile
     * @return Populated FixGatewaySeqConfiguration and created logger.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static std::tuple<FixGatewaySeqConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>> load_and_init_logging(const std::string& file_path,
                                                                                                                     const std::string& log_file_path);
};

} // namespace sample_fix_gateway_seq
