// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "WitnessConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace witness {

WitnessConfiguration WitnessConfigurationLoader::load(const std::string& file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    WitnessConfiguration config;

    try {
        toml.get_required_except("network.listen_host", config.listen_host);

        int32_t listen_port = 0;
        toml.get_required_except("network.listen_port", listen_port);

        if (listen_port < 1 || listen_port > 65535) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: network.listen_port must be in range [1, 65535], got " +
                                                        std::to_string(listen_port));
        }
        config.listen_port = static_cast<uint16_t>(listen_port);

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: logging.syslog_level '" + syslog_level_str +
                                                        "' is not a recognised log level");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_reserve_cpu0", config.cpu_pinning_reserve_cpu0);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);
        toml.get_required_except("reactor.connect_retry_warning_interval", config.connect_retry_warning_interval);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("WitnessConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespaces
