// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEnginePublisherConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace matching_engine_publisher {

MatchingEnginePublisherConfiguration MatchingEnginePublisherConfigurationLoader::load(const std::string& file_path) {
    pubsub_itc_fw::TomlConfiguration toml;
    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("MatchingEnginePublisherConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    MatchingEnginePublisherConfiguration config;

    auto validate_port = [&](int32_t port, const std::string& name) {
        if (port < 1 || port > 65535) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEnginePublisherConfigurationLoader: " + name + " must be in range [1, 65535], got " + std::to_string(port));
        }
    };

    try {
        toml.get_required_except("network.listen_host", config.listen_host);

        int32_t sequencer_wal_port = 0;
        int32_t sequencer_wal_secondary_port = 0;
        toml.get_required_except("sequencer_wal.host", config.sequencer_wal_host);
        toml.get_required_except("sequencer_wal.port", sequencer_wal_port);
        toml.get_required_except("sequencer_wal_secondary.host", config.sequencer_wal_secondary_host);
        toml.get_required_except("sequencer_wal_secondary.port", sequencer_wal_secondary_port);
        validate_port(sequencer_wal_port, "sequencer_wal.port");
        validate_port(sequencer_wal_secondary_port, "sequencer_wal_secondary.port");
        config.sequencer_wal_port           = static_cast<uint16_t>(sequencer_wal_port);
        config.sequencer_wal_secondary_port = static_cast<uint16_t>(sequencer_wal_secondary_port);

        int32_t orders_listen_port = 0;
        int32_t er_listen_port = 0;
        toml.get_required_except("topics.orders.listen_port", orders_listen_port);
        toml.get_required_except("topics.execution_reports.listen_port", er_listen_port);
        validate_port(orders_listen_port, "topics.orders.listen_port");
        validate_port(er_listen_port, "topics.execution_reports.listen_port");
        config.orders_listen_port = static_cast<uint16_t>(orders_listen_port);
        config.er_listen_port     = static_cast<uint16_t>(er_listen_port);

        toml.get_required_except("wal.directory", config.wal_directory);
        int64_t wal_segment_size = 0;
        toml.get_required_except("wal.segment_size", wal_segment_size);
        if (wal_segment_size <= 0) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEnginePublisherConfigurationLoader: wal.segment_size must be positive");
        }
        config.wal_segment_size = static_cast<size_t>(wal_segment_size);
        int32_t snapshot_interval_seconds = 0;
        toml.get_required_except("wal.snapshot_interval_seconds", snapshot_interval_seconds);
        if (snapshot_interval_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("MatchingEnginePublisherConfigurationLoader: wal.snapshot_interval_seconds must be positive");
        }
        config.snapshot_interval_seconds = snapshot_interval_seconds;

        toml.get_required_except("ha.ha_enabled", config.ha_enabled);
        toml.get_required_except("ha.instance_id", config.instance_id);

        if (config.ha_enabled) {
            toml.get_required_except("ha.arbiter_primary_host", config.arbiter_primary_host);
            int32_t arbiter_primary_port = 0;
            toml.get_required_except("ha.arbiter_primary_port", arbiter_primary_port);
            validate_port(arbiter_primary_port, "ha.arbiter_primary_port");
            config.arbiter_primary_port = static_cast<uint16_t>(arbiter_primary_port);

            toml.get_required_except("ha.arbiter_secondary_host", config.arbiter_secondary_host);
            int32_t arbiter_secondary_port = 0;
            toml.get_required_except("ha.arbiter_secondary_port", arbiter_secondary_port);
            validate_port(arbiter_secondary_port, "ha.arbiter_secondary_port");
            config.arbiter_secondary_port = static_cast<uint16_t>(arbiter_secondary_port);

            int32_t arbitration_timeout_seconds = 0;
            toml.get_required_except("ha.arbitration_timeout_seconds", arbitration_timeout_seconds);
            if (arbitration_timeout_seconds <= 0) {
                throw pubsub_itc_fw::ConfigurationException(
                    "MatchingEnginePublisherConfigurationLoader: ha.arbitration_timeout_seconds must be positive");
            }
            config.arbitration_timeout_seconds = arbitration_timeout_seconds;

            toml.get_required_except("ha.peer_listen_host", config.peer_listen_host);
            toml.get_required_except("ha.peer_host", config.peer_host);
            toml.get_required_except("ha.fence_file_path", config.fence_file_path);

            int32_t peer_listen_port = 0;
            int32_t peer_port = 0;
            int32_t heartbeat_interval_seconds = 0;
            int32_t heartbeat_timeout_seconds = 0;
            int32_t startup_election_timeout_seconds = 0;
            toml.get_required_except("ha.peer_listen_port", peer_listen_port);
            toml.get_required_except("ha.peer_port", peer_port);
            toml.get_required_except("ha.heartbeat_interval_seconds", heartbeat_interval_seconds);
            toml.get_required_except("ha.heartbeat_timeout_seconds", heartbeat_timeout_seconds);
            toml.get_required_except("ha.startup_election_timeout_seconds", startup_election_timeout_seconds);
            validate_port(peer_listen_port, "ha.peer_listen_port");
            validate_port(peer_port, "ha.peer_port");
            if (heartbeat_interval_seconds <= 0) {
                throw pubsub_itc_fw::ConfigurationException(
                    "MatchingEnginePublisherConfigurationLoader: ha.heartbeat_interval_seconds must be positive");
            }
            if (heartbeat_timeout_seconds <= 0) {
                throw pubsub_itc_fw::ConfigurationException(
                    "MatchingEnginePublisherConfigurationLoader: ha.heartbeat_timeout_seconds must be positive");
            }
            config.peer_listen_port                = static_cast<uint16_t>(peer_listen_port);
            config.peer_port                       = static_cast<uint16_t>(peer_port);
            config.heartbeat_interval_seconds      = heartbeat_interval_seconds;
            config.heartbeat_timeout_seconds       = heartbeat_timeout_seconds;
            if (startup_election_timeout_seconds > 0) {
                config.startup_election_timeout_seconds = startup_election_timeout_seconds;
            }
        }

        int64_t max_lag_records = 0;
        toml.get_required_except("subscriber_lag.default_max_lag_records", max_lag_records);
        if (max_lag_records <= 0) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEnginePublisherConfigurationLoader: subscriber_lag.default_max_lag_records must be positive");
        }
        config.max_lag_records = max_lag_records;

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);
        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEnginePublisherConfigurationLoader: logging.applog_level '" + applog_level_str + "' unrecognised");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException(
                "MatchingEnginePublisherConfigurationLoader: logging.syslog_level '" + syslog_level_str + "' unrecognised");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_reserve_cpu0", config.cpu_pinning_reserve_cpu0);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);
        toml.get_required_except("reactor.connect_retry_warning_interval", config.connect_retry_warning_interval);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespaces
