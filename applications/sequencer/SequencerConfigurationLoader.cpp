// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerConfigurationLoader.hpp"

#include <tuple>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace sequencer {

SequencerConfiguration SequencerConfigurationLoader::load(const std::string& file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    SequencerConfiguration config;

    try {
        toml.get_required_except("network.listen_host", config.listen_host);
        toml.get_required_except("network.er_listen_host", config.er_listen_host);
        toml.get_required_except("gateway.host", config.gateway_host);
        toml.get_required_except("matching_engine.host", config.matching_engine_host);
        toml.get_required_except("ha.ha_enabled", config.ha_enabled);

        int32_t listen_port = 0;
        int32_t er_listen_port = 0;
        int32_t gateway_port = 0;
        int32_t matching_engine_port = 0;

        toml.get_required_except("network.listen_port", listen_port);
        toml.get_required_except("network.er_listen_port", er_listen_port);
        toml.get_required_except("gateway.port", gateway_port);
        toml.get_required_except("matching_engine.port", matching_engine_port);
        toml.get_required_except("ha.instance_id", config.instance_id);

        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                            std::to_string(port));
            }
        };

        validate_port(listen_port, "network.listen_port");
        validate_port(er_listen_port, "network.er_listen_port");
        validate_port(gateway_port, "gateway.port");
        validate_port(matching_engine_port, "matching_engine.port");

        config.listen_port = static_cast<uint16_t>(listen_port);
        config.er_listen_port = static_cast<uint16_t>(er_listen_port);
        config.gateway_port = static_cast<uint16_t>(gateway_port);
        config.matching_engine_port = static_cast<uint16_t>(matching_engine_port);

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
                throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: ha.arbitration_timeout_seconds must be positive");
            }
            config.arbitration_timeout_seconds = arbitration_timeout_seconds;

            toml.get_required_except("peer.listen_host", config.peer_listen_host);
            toml.get_required_except("peer.host", config.peer_host);
            toml.get_required_except("peer.fence_file_path", config.fence_file_path);

            int32_t peer_listen_port = 0;
            int32_t peer_port = 0;
            int32_t heartbeat_interval_seconds = 0;
            int32_t heartbeat_timeout_seconds = 0;
            int32_t startup_election_timeout_seconds = 0;
            toml.get_required_except("peer.listen_port", peer_listen_port);
            toml.get_required_except("peer.port", peer_port);
            toml.get_required_except("peer.heartbeat_interval_seconds", heartbeat_interval_seconds);
            toml.get_required_except("peer.heartbeat_timeout_seconds", heartbeat_timeout_seconds);
            toml.get_required_except("peer.startup_election_timeout_seconds", startup_election_timeout_seconds);

            validate_port(peer_listen_port, "peer.listen_port");
            validate_port(peer_port, "peer.port");

            if (heartbeat_interval_seconds <= 0) {
                throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: peer.heartbeat_interval_seconds must be positive");
            }
            if (heartbeat_timeout_seconds <= 0) {
                throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: peer.heartbeat_timeout_seconds must be positive");
            }

            config.peer_listen_port = static_cast<uint16_t>(peer_listen_port);
            config.peer_port = static_cast<uint16_t>(peer_port);
            config.heartbeat_interval_seconds = heartbeat_interval_seconds;
            config.heartbeat_timeout_seconds = heartbeat_timeout_seconds;
            if (startup_election_timeout_seconds > 0) {
                config.startup_election_timeout_seconds = startup_election_timeout_seconds;
            }
        }

        toml.get_required_except("wal.directory", config.wal_directory);
        int64_t segment_size_bytes = 0;
        toml.get_required_except("wal.segment_size", segment_size_bytes);
        if (segment_size_bytes <= 0) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: wal.segment_size must be positive");
        }
        config.wal_segment_size = static_cast<size_t>(segment_size_bytes);

        int32_t snapshot_interval_seconds = 0;
        toml.get_required_except("wal.snapshot_interval_seconds", snapshot_interval_seconds);
        if (snapshot_interval_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: wal.snapshot_interval_seconds must be positive");
        }
        config.snapshot_interval_seconds = snapshot_interval_seconds;

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);

        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: logging.syslog_level '" + syslog_level_str +
                                                        "' is not a recognised log level");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_dev_mode", config.cpu_pinning_dev_mode);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_objects_per_slab));
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: event_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.event_queue_pool_initial_slabs));
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_objects_per_slab));
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("SequencerConfigurationLoader: command_queue_pool.initial_slabs must be >= 1, got " +
                                                        std::to_string(config.command_queue_pool_initial_slabs));
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace sequencer
