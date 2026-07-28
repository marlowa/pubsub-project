// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "BinaryGatewayConfigurationLoader.hpp"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace binary_gateway {

std::tuple<BinaryGatewayConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>>
BinaryGatewayConfigurationLoader::load_and_init_logging(const std::string& file_path, const std::string& log_file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, error] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("BinaryGatewayConfigurationLoader: failed to load '" + file_path + "': " + error);
    }

    BinaryGatewayConfiguration config;

    config.rolling_logfile_configuration = pubsub_itc_fw::LoggingConfigurationLoader::load(toml);

    auto logger =
        std::make_unique<pubsub_itc_fw::QuillLogger>(log_file_path, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                     pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info, config.rolling_logfile_configuration);

    auto validate_port = [](int32_t port, const std::string& name) {
        if (port < 1 || port > 65535) {
            throw pubsub_itc_fw::ConfigurationException("BinaryGatewayConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                        std::to_string(port));
        }
    };
    auto validate_positive = [](int32_t value, const std::string& name) {
        if (value < 1) {
            throw pubsub_itc_fw::ConfigurationException("BinaryGatewayConfigurationLoader: " + name + " must be >= 1, got " + std::to_string(value));
        }
    };

    toml.get_required_except("network.listen_host", config.listen_host);
    toml.get_required_except("network.er_listen_host", config.er_listen_host);
    toml.get_required_except("sequencer.ha_enabled", config.ha_enabled);
    toml.get_required_except("sequencer.primary_host", config.sequencer_primary_host);

    int32_t listen_port = 0;
    int32_t er_listen_port = 0;
    int32_t primary_port = 0;
    toml.get_required_except("network.listen_port", listen_port);
    toml.get_required_except("network.er_listen_port", er_listen_port);
    toml.get_required_except("sequencer.primary_port", primary_port);

    validate_port(listen_port, "network.listen_port");
    validate_port(er_listen_port, "network.er_listen_port");
    validate_port(primary_port, "sequencer.primary_port");

    config.listen_port = static_cast<uint16_t>(listen_port);
    config.er_listen_port = static_cast<uint16_t>(er_listen_port);
    config.sequencer_primary_port = static_cast<uint16_t>(primary_port);

    if (config.ha_enabled) {
        toml.get_required_except("sequencer.secondary_host", config.sequencer_secondary_host);
        int32_t secondary_port = 0;
        toml.get_required_except("sequencer.secondary_port", secondary_port);
        validate_port(secondary_port, "sequencer.secondary_port");
        config.sequencer_secondary_port = static_cast<uint16_t>(secondary_port);
    }

    std::string applog_level_text;
    std::string syslog_level_text;
    toml.get_required_except("logging.applog_level", applog_level_text);
    toml.get_required_except("logging.syslog_level", syslog_level_text);
    if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_text, config.applog_level)) {
        throw pubsub_itc_fw::ConfigurationException("BinaryGatewayConfigurationLoader: logging.applog_level '" + applog_level_text +
                                                    "' is not a recognised log level");
    }
    if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_text, config.syslog_level)) {
        throw pubsub_itc_fw::ConfigurationException("BinaryGatewayConfigurationLoader: logging.syslog_level '" + syslog_level_text +
                                                    "' is not a recognised log level");
    }

    toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
    toml.get_required_except("reactor.cpu_pinning_reserve_cpu0", config.cpu_pinning_reserve_cpu0);
    // Mandatory only when pinning is on: without both paths the CpuRegistry
    // cannot coordinate with the other processes on this machine.
    if (config.cpu_pinning_enabled) {
        toml.get_required_except("reactor.cpu_registry_shm_path", config.cpu_registry_shm_path);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);
        toml.get_required_except("reactor.cpu_layout_file", config.cpu_layout_file);
        toml.get_required_except("reactor.cpu_layout_component", config.cpu_layout_component);
    }
    toml.get_required_except("reactor.connect_retry_warning_interval", config.connect_retry_warning_interval);

    toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
    toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
    validate_positive(config.event_queue_pool_objects_per_slab, "event_queue_pool.objects_per_slab");
    validate_positive(config.event_queue_pool_initial_slabs, "event_queue_pool.initial_slabs");

    toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
    toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
    validate_positive(config.command_queue_pool_objects_per_slab, "command_queue_pool.objects_per_slab");
    validate_positive(config.command_queue_pool_initial_slabs, "command_queue_pool.initial_slabs");

    toml.get_required_except("authentication_service.host", config.authentication_service_host);
    int32_t authentication_service_port = 0;
    toml.get_required_except("authentication_service.port", authentication_service_port);
    validate_port(authentication_service_port, "authentication_service.port");
    config.authentication_service_port = static_cast<uint16_t>(authentication_service_port);
    if (config.ha_enabled) {
        toml.get_required_except("authentication_service.secondary_host", config.authentication_service_secondary_host);
        int32_t secondary = 0;
        toml.get_required_except("authentication_service.secondary_port", secondary);
        validate_port(secondary, "authentication_service.secondary_port");
        config.authentication_service_secondary_port = static_cast<uint16_t>(secondary);
    }
    toml.get_required_except("binary_session.sender_comp_id", config.sender_comp_id);
    toml.get_required_except("timeouts.scram_auth_timeout", config.scram_auth_timeout);

    toml.get_required_except("open_order_pool.objects_per_pool", config.open_order_pool_objects_per_pool);
    toml.get_required_except("open_order_pool.initial_pools", config.open_order_pool_initial_pools);
    validate_positive(config.open_order_pool_objects_per_pool, "open_order_pool.objects_per_pool");
    validate_positive(config.open_order_pool_initial_pools, "open_order_pool.initial_pools");

    return std::make_tuple(std::move(config), std::move(logger));
}

} // namespaces
