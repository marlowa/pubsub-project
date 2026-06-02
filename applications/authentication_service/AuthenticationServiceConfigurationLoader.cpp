// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include <scram_crypto/ScramCrypto.hpp>

#include "AuthenticationServiceConfigurationLoader.hpp"

namespace authentication_service {

namespace {

std::vector<uint8_t> hex_decode(std::string_view hex, std::string_view field_name) {
    if (hex.size() % 2 != 0) {
        throw pubsub_itc_fw::ConfigurationException(
            fmt::format("credentials: {} has odd-length hex string ({})", field_name, hex.size()));
    }
    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        auto nybble = [&](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            throw pubsub_itc_fw::ConfigurationException(
                fmt::format("credentials: {} contains invalid hex character '{}'", field_name, c));
        };
        result.push_back(static_cast<uint8_t>((nybble(hex[i]) << 4) | nybble(hex[i + 1])));
    }
    return result;
}

void load_credentials(const std::string& credentials_file,
                      std::unordered_map<std::string, scram_crypto::ScramCredential>& credentials) {
    pubsub_itc_fw::TomlConfiguration cred_toml;
    auto [ok, err] = cred_toml.load_file(credentials_file);
    if (!ok) {
        throw pubsub_itc_fw::ConfigurationException(
            "AuthenticationServiceConfigurationLoader: failed to load credentials file '" +
            credentials_file + "': " + err);
    }

    const std::size_t count = cred_toml.array_size("credential");
    for (std::size_t i = 0; i < count; ++i) {
        std::string comp_id;
        std::string stored_key_hex;
        std::string server_key_hex;
        std::string salt_hex;
        int32_t iterations = 0;

        cred_toml.get_required_except(fmt::format("credential[{}].comp_id",     i), comp_id);
        cred_toml.get_required_except(fmt::format("credential[{}].stored_key",  i), stored_key_hex);
        cred_toml.get_required_except(fmt::format("credential[{}].server_key",  i), server_key_hex);
        cred_toml.get_required_except(fmt::format("credential[{}].salt",        i), salt_hex);
        cred_toml.get_required_except(fmt::format("credential[{}].iterations",  i), iterations);

        scram_crypto::ScramCredential cred;
        cred.stored_key = hex_decode(stored_key_hex, fmt::format("credential[{}].stored_key", i));
        cred.server_key = hex_decode(server_key_hex, fmt::format("credential[{}].server_key", i));
        cred.salt       = hex_decode(salt_hex,       fmt::format("credential[{}].salt", i));
        cred.iterations = iterations;

        if (cred.stored_key.size() != 32) {
            throw pubsub_itc_fw::ConfigurationException(
                fmt::format("credentials: credential[{}].stored_key must be 64 hex chars (32 bytes), got {} chars",
                            i, stored_key_hex.size()));
        }
        if (cred.server_key.size() != 32) {
            throw pubsub_itc_fw::ConfigurationException(
                fmt::format("credentials: credential[{}].server_key must be 64 hex chars (32 bytes), got {} chars",
                            i, server_key_hex.size()));
        }
        if (cred.salt.size() != 16) {
            throw pubsub_itc_fw::ConfigurationException(
                fmt::format("credentials: credential[{}].salt must be 32 hex chars (16 bytes), got {} chars",
                            i, salt_hex.size()));
        }

        credentials[comp_id] = std::move(cred);
    }
}

} // namespace

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
        toml.get_required_except("reactor.cpu_pinning_reserve_cpu0", config.cpu_pinning_reserve_cpu0);
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

        toml.get_required_except("credentials_file", config.credentials_file);

        auto resolve_path_relative_to_config = [&](std::string& path) {
            if (!path.empty()) {
                const std::filesystem::path p{path};
                if (p.is_relative()) {
                    path = (std::filesystem::path{file_path}.parent_path() / p).string();
                }
            }
        };

        resolve_path_relative_to_config(config.credentials_file);

        int32_t admin_listen_port = 0;
        toml.get_required_except("admin.listen_port", admin_listen_port);
        if (admin_listen_port < 1 || admin_listen_port > 65535) {
            throw pubsub_itc_fw::ConfigurationException(
                "AuthenticationServiceConfigurationLoader: admin.listen_port must be in [1, 65535], got " +
                std::to_string(admin_listen_port));
        }
        config.admin_listen_port = static_cast<uint16_t>(admin_listen_port);

        toml.get_required_except("admin.tls_certificate_path", config.admin_tls_certificate_path);
        toml.get_required_except("admin.tls_private_key_path", config.admin_tls_private_key_path);
        // ca_path and require_client_certificate are optional.
        auto [ca_ok, ca_err] = toml.get_required("admin.tls_ca_path", config.admin_tls_ca_path);
        (void)ca_ok; (void)ca_err;
        auto [req_ok, req_err] = toml.get_required("admin.tls_require_client_certificate",
                                                    config.admin_tls_require_client_certificate);
        (void)req_ok; (void)req_err;

        resolve_path_relative_to_config(config.admin_tls_certificate_path);
        resolve_path_relative_to_config(config.admin_tls_private_key_path);
        resolve_path_relative_to_config(config.admin_tls_ca_path);

    } catch (const pubsub_itc_fw::ConfigurationException&) {
        throw;
    }

    load_credentials(config.credentials_file, config.credentials);

    return std::make_tuple(std::move(config), std::move(logger));
}

} // namespace authentication_service
