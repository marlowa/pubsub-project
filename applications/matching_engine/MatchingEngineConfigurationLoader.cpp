// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace matching_engine {

MatchingEngineConfiguration MatchingEngineConfigurationLoader::load(const std::string& file_path)
{
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "MatchingEngineConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    MatchingEngineConfiguration config;

    try {
        toml.get_required_except("network.listen_host",   config.listen_host);
        toml.get_required_except("sequencer_er.host",     config.sequencer_er_host);

        int32_t listen_port      = 0;
        int32_t sequencer_er_port = 0;

        toml.get_required_except("network.listen_port",   listen_port);
        toml.get_required_except("sequencer_er.port",     sequencer_er_port);

        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException(
                    "MatchingEngineConfigurationLoader: " + name +
                    " must be in range [1, 65535], got " + std::to_string(port));
            }
        };

        validate_port(listen_port,       "network.listen_port");
        validate_port(sequencer_er_port, "sequencer_er.port");

        config.listen_port       = static_cast<uint16_t>(listen_port);
        config.sequencer_er_port = static_cast<uint16_t>(sequencer_er_port);

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEngineConfigurationLoader: logging.applog_level '" + applog_level_str
                + "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEngineConfigurationLoader: logging.syslog_level '" + syslog_level_str
                + "' is not a recognised log level");
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace matching_engine
