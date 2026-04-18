// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "FixGatewayConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace sample_fix_gateway {

FixGatewayConfiguration FixGatewayConfigurationLoader::load(const std::string& file_path)
{
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "FixGatewayConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    FixGatewayConfiguration config;

    try {
        toml.get_required_except("network.listen_host",         config.listen_host);

        int32_t listen_port = 0;
        toml.get_required_except("network.listen_port",         listen_port);

        int64_t raw_buffer_capacity = 0;
        toml.get_required_except("network.raw_buffer_capacity", raw_buffer_capacity);

        toml.get_required_except("fix_session.sender_comp_id",         config.sender_comp_id);
        toml.get_required_except("fix_session.default_target_comp_id", config.default_target_comp_id);
        toml.get_required_except("timeouts.logon_timeout",             config.logon_timeout);

        // Validate listen_port range -- TomlConfiguration has no unsigned support
        // so the port is loaded as int32_t and validated here.
        if (listen_port < 1 || listen_port > 65535) {
            throw pubsub_itc_fw::ConfigurationException(
                "FixGatewayConfigurationLoader: network.listen_port must be in range [1, 65535], got "
                + std::to_string(listen_port));
        }
        config.listen_port = static_cast<uint16_t>(listen_port);

        // Validate raw_buffer_capacity is positive.
        if (raw_buffer_capacity <= 0) {
            throw pubsub_itc_fw::ConfigurationException(
                "FixGatewayConfigurationLoader: network.raw_buffer_capacity must be positive, got "
                + std::to_string(raw_buffer_capacity));
        }
        config.raw_buffer_capacity = raw_buffer_capacity;

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw; // re-throw as-is, message already contains context
    }

    return config;
}

} // namespace sample_fix_gateway
