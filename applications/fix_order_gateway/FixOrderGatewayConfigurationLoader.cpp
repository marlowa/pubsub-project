// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/MetricsConfigurationLoader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "FixOrderGatewayConfigurationLoader.hpp"
#include "FixSession.hpp"

namespace fix_order_gateway {

std::tuple<FixOrderGatewayConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>>
FixOrderGatewayConfigurationLoader::load_and_init_logging(const std::string& file_path, const std::string& log_file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    FixOrderGatewayConfiguration config;

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
        toml.get_required_except("fix_session.sender_comp_id", config.sender_comp_id);

        // Optional, defaulting to 1: a deployment running a single instance of this
        // protocol needs no such line, and that is every deployment until step 3. The
        // sequencer reports a mismatch loudly, so a wrong value fails visibly rather
        // than by reports quietly going missing.
        int32_t instance_id = 1;
        const auto [has_instance_id, instance_id_error] = toml.get_required("gateway.instance_id", instance_id);
        if (!has_instance_id) {
            instance_id = 1;
        }
        if (instance_id < 1 || instance_id > INT16_MAX) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: gateway.instance_id must be 1 or greater "
                                                        "(instances are numbered from 1)");
        }
        config.instance_id = static_cast<int16_t>(instance_id);
        toml.get_required_except("fix_session.default_target_comp_id", config.default_target_comp_id);
        toml.get_required_except("timeouts.logon_timeout", config.logon_timeout);
        toml.get_required_except("timeouts.scram_auth_timeout", config.scram_auth_timeout);
        toml.get_required_except("cancel_on_disconnect.enabled", config.cancel_on_disconnect_enabled);
        toml.get_required_except("cancel_on_disconnect.grace_period", config.cancel_on_disconnect_grace_period);

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
                throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                            std::to_string(port));
            }
        };

        validate_port(listen_port, "network.listen_port");
        validate_port(er_listen_port, "network.er_listen_port");
        validate_port(primary_port, "sequencer.primary_port");
        validate_port(authentication_service_port, "authentication_service.port");

        if (raw_buffer_capacity <= 0) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: network.raw_buffer_capacity must be positive, got " +
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
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: logging.syslog_level '" + syslog_level_str +
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

            config.metrics_configuration = pubsub_itc_fw::MetricsConfigurationLoader::load(toml);
        }
        toml.get_required_except("reactor.connect_retry_warning_interval", config.connect_retry_warning_interval);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

        toml.get_required_except("fix_tls.enabled", config.fix_tls_enabled);
        if (config.fix_tls_enabled) {
            toml.get_required_except("fix_tls.cert", config.fix_tls_cert_path);
            toml.get_required_except("fix_tls.key", config.fix_tls_key_path);
            if (config.fix_tls_cert_path.empty()) {
                throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_tls.cert must not be empty when fix_tls.enabled=true");
            }
            if (config.fix_tls_key_path.empty()) {
                throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_tls.key must not be empty when fix_tls.enabled=true");
            }
            int32_t tls_listen_port = 0;
            toml.get_required_except("network.tls_listen_port", tls_listen_port);
            validate_port(tls_listen_port, "network.tls_listen_port");
            config.tls_listen_port = static_cast<uint16_t>(tls_listen_port);
        }

        toml.get_required_except("fix_capture.enabled", config.fix_capture_enabled);
        toml.get_required_except("fix_capture.file", config.fix_capture_file);
        toml.get_required_except("fix_capture.ring_bytes", config.fix_capture_ring_bytes);
        if (config.fix_capture_enabled && config.fix_capture_file.empty()) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_capture.file must not be empty when fix_capture.enabled=true");
        }
        if (config.fix_capture_ring_bytes < 4096) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_capture.ring_bytes must be >= 4096, got " +
                                                        std::to_string(config.fix_capture_ring_bytes));
        }

        // ClOrdID length is fixed by fix_order_limits::max_cl_ord_id_length (a hard compile-time
        // bound shared with the matching engine), so it is not configurable here.
        int32_t max_symbol_length = 0;
        int32_t max_order_qty_length = 0;
        toml.get_required_except("fix_limits.max_symbol_length", max_symbol_length);
        toml.get_required_except("fix_limits.max_order_qty_length", max_order_qty_length);
        if (max_symbol_length < 1 || static_cast<size_t>(max_symbol_length) > max_supported_symbol_length) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_limits.max_symbol_length must be in [1, " +
                                                        std::to_string(max_supported_symbol_length) + "], got " + std::to_string(max_symbol_length));
        }
        if (max_order_qty_length < 1 || static_cast<size_t>(max_order_qty_length) > max_supported_order_qty_length) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: fix_limits.max_order_qty_length must be in [1, " +
                                                        std::to_string(max_supported_order_qty_length) + "], got " + std::to_string(max_order_qty_length));
        }
        config.max_symbol_length = max_symbol_length;
        config.max_order_qty_length = max_order_qty_length;

        toml.get_required_except("open_order_pool.objects_per_pool", config.open_order_pool_objects_per_pool);
        toml.get_required_except("open_order_pool.initial_pools", config.open_order_pool_initial_pools);
        if (config.open_order_pool_objects_per_pool < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: open_order_pool.objects_per_pool must be >= 1");
        }
        if (config.open_order_pool_initial_pools < 1) {
            throw pubsub_itc_fw::ConfigurationException("FixOrderGatewayConfigurationLoader: open_order_pool.initial_pools must be >= 1");
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return std::make_tuple(std::move(config), std::move(logger));
}

} // namespaces
