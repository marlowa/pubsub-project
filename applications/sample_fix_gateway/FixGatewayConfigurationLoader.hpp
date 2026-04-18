// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

#include "FixGatewayConfiguration.hpp"

namespace sample_fix_gateway {

/**
 * @brief Loads a FixGatewayConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host         = "127.0.0.1"
 * listen_port         = 9878
 * raw_buffer_capacity = 65536
 *
 * [fix_session]
 * sender_comp_id         = "GATEWAY"
 * default_target_comp_id = "CLIENT"
 *
 * [timeouts]
 * logon_timeout = "30s"
 * @endcode
 *
 * All fields are required. If any field is missing or has the wrong type,
 * or if the file cannot be read or parsed, a ConfigurationException is thrown.
 *
 * listen_port is stored as int32_t in TOML (no unsigned support) and validated
 * to be in the range [1, 65535].
 */
class FixGatewayConfigurationLoader {
public:
    /**
     * @brief Loads and validates configuration from the given TOML file path.
     *
     * @param file_path Path to the TOML configuration file.
     * @return Populated FixGatewayConfiguration.
     * @throws pubsub_itc_fw::ConfigurationException if the file cannot be
     *         read, parsed, or if any required field is missing, has the
     *         wrong type, or fails validation.
     */
    static FixGatewayConfiguration load(const std::string& file_path);
};

} // namespace sample_fix_gateway
