// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "FixGatewaySeqConfigurationLoader.hpp"

namespace sample_fix_gateway_seq {

std::tuple<FixGatewaySeqConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>>
FixGatewaySeqConfigurationLoader::load_and_init_logging(const std::string& file_path, const std::string& log_file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    FixGatewaySeqConfiguration config;

    // Get the logger going early
    try {
        config.rolling_logfile_configuration = pubsub_itc_fw::LoggingConfigurationLoader::load(toml);
    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    auto logger =
        std::make_unique<pubsub_itc_fw::QuillLogger>(log_file_path, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                     pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info, config.rolling_logfile_configuration);

    try {
        toml.get_required_except("network.listen_host", config.listen_host);
        toml.get_required_except("network.er_listen_host", config.er_listen_host);
        toml.get_required_except("authentication_service.host", config.authentication_service_host);
        toml.get_required_except("authentication_service.scram_password", config.scram_password);
        toml.get_required_except("fix_session.sender_comp_id", config.sender_comp_id);
        toml.get_required_except("fix_session.default_target_comp_id", config.default_target_comp_id);
        toml.get_required_except("timeouts.logon_timeout", config.logon_timeout);
        toml.get_required_except("timeouts.scram_auth_timeout", config.scram_auth_timeout);

        toml.get_required_except("sequencer.ha_enabled", config.ha_enabled);

        toml.get_required_except("sequencer.primary_host", config.sequencer_primary_host);

        int32_t listen_port = 0;
        int32_t er_listen_port = 0;
        int32_t primary_port = 0;
        int32_t authentication_service_port = 0;
        int64_t raw_buffer_capacity = 0;

        toml.get_required_except("network.listen_port", listen_port);
        toml.get_required_except("network.er_listen_port", er_listen_port);
        toml.get_required_except("network.raw_buffer_capacity", raw_buffer_capacity);
        toml.get_required_except("sequencer.primary_port", primary_port);
        toml.get_required_except("authentication_service.port", authentication_service_port);

        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                            std::to_string(port));
            }
        };

        validate_port(listen_port, "network.listen_port");
        validate_port(er_listen_port, "network.er_listen_port");
        validate_port(primary_port, "sequencer.primary_port");
        validate_port(authentication_service_port, "authentication_service.port");

        if (raw_buffer_capacity <= 0) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: network.raw_buffer_capacity must be positive, got " +
                                                        std::to_string(raw_buffer_capacity));
        }

        config.listen_port = static_cast<uint16_t>(listen_port);
        config.er_listen_port = static_cast<uint16_t>(er_listen_port);
        config.sequencer_primary_port = static_cast<uint16_t>(primary_port);
        config.authentication_service_port = static_cast<uint16_t>(authentication_service_port);
        config.raw_buffer_capacity = raw_buffer_capacity;

        if (config.ha_enabled) {
            toml.get_required_except("sequencer.secondary_host", config.sequencer_secondary_host);
            int32_t secondary_port = 0;
            toml.get_required_except("sequencer.secondary_port", secondary_port);
            validate_port(secondary_port, "sequencer.secondary_port");
            config.sequencer_secondary_port = static_cast<uint16_t>(secondary_port);

            toml.get_required_except("authentication_service.secondary_host", config.authentication_service_secondary_host);
            int32_t authentication_service_secondary_port = 0;
            toml.get_required_except("authentication_service.secondary_port", authentication_service_secondary_port);
            validate_port(authentication_service_secondary_port, "authentication_service.secondary_port");
            config.authentication_service_secondary_port = static_cast<uint16_t>(authentication_service_secondary_port);
        }

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: logging.syslog_level '" + syslog_level_str +
                                                        "' is not a recognised log level");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_dev_mode", config.cpu_pinning_dev_mode);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixGatewaySeqConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return std::make_tuple(std::move(config), std::move(logger));
}

} // namespace sample_fix_gateway_seq
