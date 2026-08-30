// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/MetricsConfigurationLoader.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace matching_engine {

MatchingEngineConfiguration MatchingEngineConfigurationLoader::load(const std::string& file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    MatchingEngineConfiguration config;

    try {
        // HA role must be read first -- it controls which other sections are required.
        toml.get_required_except("ha.enabled", config.ha_enabled);
        if (config.ha_enabled) {
            toml.get_required_except("ha.role", config.ha_role);
            if (config.ha_role != "primary" && config.ha_role != "secondary") {
                throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: ha.role must be 'primary' or 'secondary', got '" +
                                                            config.ha_role + "'");
            }
        }

        // No role check here any more: every HA section is read identically for both roles.
        // Replication used to be the exception -- the primary read only the dial half and the
        // secondary only the listen half -- and that asymmetry is what left no channel in the
        // direction replication needed after a failover.

        // Network/sequencer sections are required for both roles.
        // The secondary needs them for pre-warmed connections (Slice C): it must
        // register the same order listener and sequencer_er outbound services as
        // the primary so that on promotion it can begin processing without a
        // cold-connect delay.
        {
            toml.get_required_except("network.listen_host", config.listen_host);
            toml.get_required_except("sequencer_er.host", config.sequencer_er_host);
            toml.get_required_except("sequencer_er_secondary.host", config.sequencer_er_secondary_host);

            int32_t listen_port = 0;
            int32_t sequencer_er_port = 0;
            int32_t sequencer_er_secondary_port = 0;

            toml.get_required_except("network.listen_port", listen_port);
            toml.get_required_except("sequencer_er.port", sequencer_er_port);
            toml.get_required_except("sequencer_er_secondary.port", sequencer_er_secondary_port);

            auto validate_port = [&](int32_t port, const std::string& name) {
                if (port < 1 || port > 65535) {
                    throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                                std::to_string(port));
                }
            };

            validate_port(listen_port, "network.listen_port");
            validate_port(sequencer_er_port, "sequencer_er.port");
            validate_port(sequencer_er_secondary_port, "sequencer_er_secondary.port");

            config.listen_port = static_cast<uint16_t>(listen_port);
            config.sequencer_er_port = static_cast<uint16_t>(sequencer_er_port);
            config.sequencer_er_secondary_port = static_cast<uint16_t>(sequencer_er_secondary_port);
        }

        // Book replication configuration (required when ha_enabled).
        if (config.ha_enabled) {
            auto validate_port = [&](int32_t port, const std::string& name) {
                if (port < 1 || port > 65535) {
                    throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                                std::to_string(port));
                }
            };
            // Both halves, for both roles. Replication used to be read one way round -- the
            // primary only dialled, the secondary only listened -- which left no channel at
            // all in the direction replication needed after a failover. Both instances now
            // listen and both dial; which connection carries book updates follows the ROLE,
            // because the leader is the sender.
            toml.get_required_except("book_replication.host", config.peer_replication_host);
            int32_t repl_port = 0;
            toml.get_required_except("book_replication.port", repl_port);
            validate_port(repl_port, "book_replication.port");
            config.peer_replication_port = static_cast<uint16_t>(repl_port);

            toml.get_required_except("book_replication.listen_host", config.replication_listen_host);
            int32_t repl_listen_port = 0;
            toml.get_required_except("book_replication.listen_port", repl_listen_port);
            validate_port(repl_listen_port, "book_replication.listen_port");
            config.replication_listen_port = static_cast<uint16_t>(repl_listen_port);

            // Arbiter-mediated promotion (Slice C+D). Required for both roles when HA is on.
            toml.get_required_except("ha_instance.instance_id", config.instance_id);
            toml.get_required_except("ha_instance.epoch_state_file", config.epoch_state_file);

            toml.get_required_except("arbiter_primary.host", config.arbiter_primary_host);
            int32_t arbiter_primary_port = 0;
            toml.get_required_except("arbiter_primary.port", arbiter_primary_port);
            validate_port(arbiter_primary_port, "arbiter_primary.port");
            config.arbiter_primary_port = static_cast<uint16_t>(arbiter_primary_port);

            toml.get_required_except("arbiter_secondary.host", config.arbiter_secondary_host);
            int32_t arbiter_secondary_port = 0;
            toml.get_required_except("arbiter_secondary.port", arbiter_secondary_port);
            validate_port(arbiter_secondary_port, "arbiter_secondary.port");
            config.arbiter_secondary_port = static_cast<uint16_t>(arbiter_secondary_port);

            toml.get_required_except("ha_timing.heartbeat_timeout_seconds", config.heartbeat_timeout_seconds);
            toml.get_required_except("ha_timing.heartbeat_interval_seconds", config.heartbeat_interval_seconds);
            if (config.heartbeat_timeout_seconds < 1) {
                throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: ha_timing.heartbeat_timeout_seconds must be >= 1, got " +
                                                            std::to_string(config.heartbeat_timeout_seconds));
            }
            if (config.heartbeat_interval_seconds < 1) {
                throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: ha_timing.heartbeat_interval_seconds must be >= 1, got " +
                                                            std::to_string(config.heartbeat_interval_seconds));
            }
        }

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: logging.syslog_level '" + syslog_level_str +
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

        // Outside the cpu_pinning_enabled block above: metrics and CPU pinning are unrelated
        // concerns, and the test harnesses read their ground truth from these counters, so a
        // component must expose them whether or not it is pinned.
        config.metrics_configuration = pubsub_itc_fw::MetricsConfigurationLoader::load(toml);
        toml.get_required_except("reactor.connect_retry_warning_interval", config.connect_retry_warning_interval);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

        toml.get_required_except("order_book.initial_capacity", config.order_book_initial_capacity);
        // Optional: an existing deployment that predates this keeps the default rather
        // than failing to start, and the default is a useful value rather than "off".
        static_cast<void>(toml.get_required("order_book.growth_report_threshold_bytes", config.order_book_growth_report_threshold_bytes));
        toml.get_required_except("order_book.region_path", config.order_book_region_path);
        toml.get_required_except("order_book.region_capacity", config.order_book_region_capacity);
        if (config.order_book_region_path.empty()) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: order_book.region_path must name a file");
        }
        if (config.order_book_region_capacity < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: order_book.region_capacity must be >= 1, got " +
                                                        std::to_string(config.order_book_region_capacity));
        }
        if (config.order_book_initial_capacity < 1) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEngineConfigurationLoader: order_book.initial_capacity must be >= 1, got " +
                                                        std::to_string(config.order_book_initial_capacity));
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespaces
