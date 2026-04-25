// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewaySeqConfigurationLoader.hpp"

#include <string>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace sample_fix_gateway_seq {

FixGatewaySeqConfiguration FixGatewaySeqConfigurationLoader::load(const std::string& file_path)
{
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "FixGatewaySeqConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    FixGatewaySeqConfiguration config;

    try {
        toml.get_required_except("network.listen_host",         config.listen_host);
        toml.get_required_except("network.er_listen_host",      config.er_listen_host);
        toml.get_required_except("fix_session.sender_comp_id",         config.sender_comp_id);
        toml.get_required_except("fix_session.default_target_comp_id", config.default_target_comp_id);
        toml.get_required_except("timeouts.logon_timeout",             config.logon_timeout);

        toml.get_required_except("sequencer.primary_host",   config.sequencer_primary_host);
        toml.get_required_except("sequencer.secondary_host", config.sequencer_secondary_host);

        int32_t listen_port = 0;
        int32_t er_listen_port = 0;
        int32_t primary_port = 0;
        int32_t secondary_port = 0;
        int64_t raw_buffer_capacity = 0;

        toml.get_required_except("network.listen_port",         listen_port);
        toml.get_required_except("network.er_listen_port",      er_listen_port);
        toml.get_required_except("network.raw_buffer_capacity", raw_buffer_capacity);
        toml.get_required_except("sequencer.primary_port",      primary_port);
        toml.get_required_except("sequencer.secondary_port",    secondary_port);

        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException(
                    "FixGatewaySeqConfigurationLoader: " + name +
                    " must be in range [1, 65535], got " + std::to_string(port));
            }
        };

        validate_port(listen_port,    "network.listen_port");
        validate_port(er_listen_port, "network.er_listen_port");
        validate_port(primary_port,   "sequencer.primary_port");
        validate_port(secondary_port, "sequencer.secondary_port");

        if (raw_buffer_capacity <= 0) {
            throw pubsub_itc_fw::ConfigurationException(
                "FixGatewaySeqConfigurationLoader: network.raw_buffer_capacity must be positive, got "
                + std::to_string(raw_buffer_capacity));
        }

        config.listen_port              = static_cast<uint16_t>(listen_port);
        config.er_listen_port           = static_cast<uint16_t>(er_listen_port);
        config.sequencer_primary_port   = static_cast<uint16_t>(primary_port);
        config.sequencer_secondary_port = static_cast<uint16_t>(secondary_port);
        config.raw_buffer_capacity      = raw_buffer_capacity;

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace sample_fix_gateway_seq
