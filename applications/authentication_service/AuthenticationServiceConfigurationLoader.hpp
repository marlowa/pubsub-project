#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "AuthenticationServiceConfiguration.hpp"

namespace authentication_service {

/**
 * @brief Loads an AuthenticationServiceConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * credentials_file = "credentials.toml"
 *
 * [network]
 * listen_host = "127.0.0.1"
 * listen_port = 11070
 * connection_inactivity_limit = "600s"
 *
 * [admin]
 * listen_port                    = 11072
 * tls_certificate_path           = "admin.crt"
 * tls_private_key_path           = "admin.key"
 * tls_ca_path                    = ""
 * tls_require_client_certificate = false
 *
 * [logging]
 * applog_level     = "info"
 * syslog_level     = "critical"
 * mode             = "none"
 * max_file_size    = 10240000
 * max_backup_files = 10
 *
 * [reactor]
 * cpu_pinning_enabled            = false
 * cpu_pinning_reserve_cpu0       = true
 * connect_retry_warning_interval = "15m"
 *
 * [event_queue_pool]
 * objects_per_slab = 64
 * initial_slabs    = 1
 *
 * [command_queue_pool]
 * objects_per_slab = 64
 * initial_slabs    = 1
 * @endcode
 *
 * With cpu_pinning_enabled true, [reactor] also needs cpu_registry_shm_path, cpu_registry_lock_file, cpu_layout_file
 * and cpu_layout_component, and a [metrics] section is read. The two admin keys tls_ca_path and
 * tls_require_client_certificate are optional; every other key shown is required. Throws ConfigurationException on any
 * error.
 *
 * network.connection_inactivity_limit is how long a connection may send nothing before the service closes it. It must
 * be longer than the order gateways' timeouts.authentication_heartbeat_interval, or the service closes connections
 * that are working.
 */
class AuthenticationServiceConfigurationLoader {
  public:
    /**
     * @brief Reads and checks the configuration from a TOML document already parsed, and loads the credentials file it
     *        names.
     *
     * Separate from load_and_init_logging() so that a test can give it a document of its own without a logger being
     * made.
     *
     * @param[in] toml      The parsed configuration.
     * @param[in] file_path The path the configuration was read from. Relative paths in it, the credentials file and the
     *                      administration listener's certificate files, are taken relative to the directory holding it.
     * @return The configuration, with the credentials and session policies loaded.
     */
    static AuthenticationServiceConfiguration load(const pubsub_itc_fw::TomlConfiguration& toml, const std::string& file_path);

    /**
     * @brief Loads and validates configuration from the given TOML file path and
     *        initialises the logger using the [logging] section.
     *
     * @param[in] file_path     Path to the TOML configuration file.
     * @param[in] log_file_path Pathname for the application log file.
     * @return Populated AuthenticationServiceConfiguration and initialised logger.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static std::tuple<AuthenticationServiceConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>> load_and_init_logging(const std::string& file_path,
                                                                                                                             const std::string& log_file_path);
};

} // namespaces
