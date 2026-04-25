// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace sequencer {

SequencerConfiguration SequencerConfigurationLoader::load(const std::string& file_path)
{
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "SequencerConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    SequencerConfiguration config;

    try {
        toml.get_required_except("network.listen_host",    config.listen_host);
        toml.get_required_except("matching_engine.host",   config.matching_engine_host);
        toml.get_required_except("ha.peer_host",           config.peer_host);
        toml.get_required_except("ha.arbiter_host",        config.arbiter_host);

        int32_t listen_port = 0;
        int32_t me_port     = 0;
        int32_t peer_port   = 0;
        int32_t arbiter_port = 0;

        toml.get_required_except("network.listen_port",    listen_port);
        toml.get_required_except("matching_engine.port",   me_port);
        toml.get_required_except("ha.instance_id",         config.instance_id);
        toml.get_required_except("ha.peer_port",           peer_port);
        toml.get_required_except("ha.arbiter_port",        arbiter_port);

        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException(
                    "SequencerConfigurationLoader: " + name +
                    " must be in range [1, 65535], got " + std::to_string(port));
            }
        };

        validate_port(listen_port,   "network.listen_port");
        validate_port(me_port,       "matching_engine.port");
        validate_port(peer_port,     "ha.peer_port");
        validate_port(arbiter_port,  "ha.arbiter_port");

        config.listen_port          = static_cast<uint16_t>(listen_port);
        config.matching_engine_port = static_cast<uint16_t>(me_port);
        config.peer_port            = static_cast<uint16_t>(peer_port);
        config.arbiter_port         = static_cast<uint16_t>(arbiter_port);

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace sequencer
