// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace arbiter {

ArbiterConfiguration ArbiterConfigurationLoader::load(const std::string& file_path)
{
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "ArbiterConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    ArbiterConfiguration config;

    try {
        toml.get_required_except("network.listen_host", config.listen_host);

        int32_t listen_port = 0;
        toml.get_required_except("network.listen_port", listen_port);

        if (listen_port < 1 || listen_port > 65535) {
            throw pubsub_itc_fw::ConfigurationException(
                "ArbiterConfigurationLoader: network.listen_port must be in range [1, 65535], got "
                + std::to_string(listen_port));
        }
        config.listen_port = static_cast<uint16_t>(listen_port);

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "ArbiterConfigurationLoader: logging.applog_level '" + applog_level_str
                + "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "ArbiterConfigurationLoader: logging.syslog_level '" + syslog_level_str
                + "' is not a recognised log level");
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace arbiter
