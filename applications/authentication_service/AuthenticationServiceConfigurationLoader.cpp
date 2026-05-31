// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "AuthenticationServiceConfigurationLoader.hpp"

namespace authentication_service {

std::tuple<AuthenticationServiceConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>>
AuthenticationServiceConfigurationLoader::load_and_init_logging(const std::string& file_path, const std::string& log_file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    AuthenticationServiceConfiguration config;

    try {
        config.rolling_logfile_configuration = pubsub_itc_fw::LoggingConfigurationLoader::load(toml);
    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(log_file_path, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                                pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info,
                                                                config.rolling_logfile_configuration);

    try {
        toml.get_required_except("network.listen_host", config.listen_host);

        int32_t listen_port = 0;
        toml.get_required_except("network.listen_port", listen_port);
        if (listen_port < 1 || listen_port > 65535) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: network.listen_port must be in [1, 65535], got " +
                                                        std::to_string(listen_port));
        }
        config.listen_port = static_cast<uint16_t>(listen_port);

        int64_t raw_buffer_capacity = 0;
        toml.get_required_except("network.raw_buffer_capacity", raw_buffer_capacity);
        if (raw_buffer_capacity <= 0) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: network.raw_buffer_capacity must be positive, got " +
                                                        std::to_string(raw_buffer_capacity));
        }
        config.raw_buffer_capacity = raw_buffer_capacity;

        toml.get_required_except("tls.certificate_path", config.certificate_path);
        toml.get_required_except("tls.private_key_path", config.private_key_path);
        toml.get_required_except("tls.ca_path", config.ca_path);
        toml.get_required_except("tls.require_client_certificate", config.require_client_certificate);

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);
        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: logging.syslog_level '" + syslog_level_str +
                                                        "' is not a recognised log level");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_dev_mode", config.cpu_pinning_dev_mode);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("AuthenticationServiceConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return std::make_tuple(std::move(config), std::move(logger));
}

} // namespace authentication_service
