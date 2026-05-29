// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ArbiterConfigurationLoader.hpp"

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace arbiter {

ArbiterConfiguration ArbiterConfigurationLoader::load(const std::string& file_path) {
    pubsub_itc_fw::TomlConfiguration toml;

    auto [ok, err] = toml.load_file(file_path);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: failed to load '" + file_path + "': " + err);
    }

    ArbiterConfiguration config;

    try {
        auto validate_port = [&](int32_t port, const std::string& name) {
            if (port < 1 || port > 65535) {
                throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: " + name + " must be in range [1, 65535], got " +
                                                            std::to_string(port));
            }
        };

        toml.get_required_except("network.listen_host", config.listen_host);
        int32_t listen_port = 0;
        toml.get_required_except("network.listen_port", listen_port);
        validate_port(listen_port, "network.listen_port");
        config.listen_port = static_cast<uint16_t>(listen_port);

        toml.get_required_except("ha.instance_id", config.instance_id);

        toml.get_required_except("peer.listen_host", config.peer_listen_host);
        int32_t peer_listen_port = 0;
        toml.get_required_except("peer.listen_port", peer_listen_port);
        validate_port(peer_listen_port, "peer.listen_port");
        config.peer_listen_port = static_cast<uint16_t>(peer_listen_port);

        toml.get_required_except("peer.host", config.peer_host);
        int32_t peer_port = 0;
        toml.get_required_except("peer.port", peer_port);
        validate_port(peer_port, "peer.port");
        config.peer_port = static_cast<uint16_t>(peer_port);

        toml.get_required_except("peer.fence_file_path", config.fence_file_path);

        int32_t heartbeat_interval_seconds = 0;
        int32_t heartbeat_timeout_seconds = 0;
        int32_t startup_election_timeout_seconds = 0;
        toml.get_required_except("peer.heartbeat_interval_seconds", heartbeat_interval_seconds);
        toml.get_required_except("peer.heartbeat_timeout_seconds", heartbeat_timeout_seconds);
        toml.get_required_except("peer.startup_election_timeout_seconds", startup_election_timeout_seconds);
        if (heartbeat_interval_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: peer.heartbeat_interval_seconds must be positive");
        }
        if (heartbeat_timeout_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: peer.heartbeat_timeout_seconds must be positive");
        }
        config.heartbeat_interval_seconds = heartbeat_interval_seconds;
        config.heartbeat_timeout_seconds = heartbeat_timeout_seconds;
        if (startup_election_timeout_seconds > 0) {
            config.startup_election_timeout_seconds = startup_election_timeout_seconds;
        }

        toml.get_required_except("witness.host", config.witness_host);
        int32_t witness_port = 0;
        toml.get_required_except("witness.port", witness_port);
        validate_port(witness_port, "witness.port");
        config.witness_port = static_cast<uint16_t>(witness_port);

        int32_t vote_timeout_seconds = 0;
        toml.get_required_except("witness.vote_timeout_seconds", vote_timeout_seconds);
        if (vote_timeout_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: witness.vote_timeout_seconds must be positive");
        }
        config.vote_timeout_seconds = vote_timeout_seconds;

        int32_t witness_heartbeat_interval_seconds = 0;
        toml.get_required_except("witness.heartbeat_interval_seconds", witness_heartbeat_interval_seconds);
        if (witness_heartbeat_interval_seconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: witness.heartbeat_interval_seconds must be positive");
        }
        config.witness_heartbeat_interval_seconds = witness_heartbeat_interval_seconds;

        std::string applog_level_str;
        std::string syslog_level_str;
        toml.get_required_except("logging.applog_level", applog_level_str);
        toml.get_required_except("logging.syslog_level", syslog_level_str);
        if (!pubsub_itc_fw::FwLogLevel::from_string(applog_level_str, config.applog_level)) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: logging.applog_level '" + applog_level_str +
                                                        "' is not a recognised log level");
        }
        if (!pubsub_itc_fw::FwLogLevel::from_string(syslog_level_str, config.syslog_level)) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: logging.syslog_level '" + syslog_level_str +
                                                        "' is not a recognised log level");
        }

        toml.get_required_except("reactor.cpu_pinning_enabled", config.cpu_pinning_enabled);
        toml.get_required_except("reactor.cpu_pinning_dev_mode", config.cpu_pinning_dev_mode);
        toml.get_required_except("reactor.cpu_registry_lock_file", config.cpu_registry_lock_file);

        toml.get_required_except("event_queue_pool.objects_per_slab", config.event_queue_pool_objects_per_slab);
        toml.get_required_except("event_queue_pool.initial_slabs", config.event_queue_pool_initial_slabs);
        if (config.event_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: event_queue_pool.objects_per_slab must be >= 1");
        }
        if (config.event_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: event_queue_pool.initial_slabs must be >= 1");
        }

        toml.get_required_except("command_queue_pool.objects_per_slab", config.command_queue_pool_objects_per_slab);
        toml.get_required_except("command_queue_pool.initial_slabs", config.command_queue_pool_initial_slabs);
        if (config.command_queue_pool_objects_per_slab < 1) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: command_queue_pool.objects_per_slab must be >= 1");
        }
        if (config.command_queue_pool_initial_slabs < 1) {
            throw pubsub_itc_fw::ConfigurationException("ArbiterConfigurationLoader: command_queue_pool.initial_slabs must be >= 1");
        }

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    return config;
}

} // namespace arbiter
