#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "FixGatewaySeqConfiguration.hpp"

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
 * secondary_host = "127.0.0.1"
 * secondary_port = 7002
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
     * @param[in] file_path Path to the TOML configuration file.
     * @return Populated FixGatewaySeqConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static FixGatewaySeqConfiguration load(const std::string& file_path);
};

} // namespace sample_fix_gateway_seq
