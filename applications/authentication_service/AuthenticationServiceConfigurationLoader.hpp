#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "AuthenticationServiceConfiguration.hpp"

namespace authentication_service {

/**
 * @brief Loads an AuthenticationServiceConfiguration from a TOML file.
 *
 * Expected TOML structure:
 * @code
 * [network]
 * listen_host         = "127.0.0.1"
 * listen_port         = 7070
 * raw_buffer_capacity = 65536
 *
 * [tls]
 * certificate_path           = "/etc/authentication_service/server.crt"
 * private_key_path           = "/etc/authentication_service/server.key"
 * ca_path                    = ""
 * require_client_certificate = false
 *
 * [logging]
 * applog_level      = "info"
 * syslog_level      = "critical"
 * mode              = "none"
 * max_file_size     = 10240000
 * max_backup_files  = 10
 *
 * [reactor]
 * cpu_pinning_enabled    = false
 * cpu_pinning_reserve_cpu0   = true
 * cpu_registry_lock_file = "/dev/shm/pubsub_cpu_registry.lock"
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
 * All fields are required. Throws ConfigurationException on any error.
 */
class AuthenticationServiceConfigurationLoader {
  public:
    /**
     * @brief Loads and validates configuration from the given TOML file path and
     *        initialises the logger using the [logging] section.
     *
     * @param[in] file_path     Path to the TOML configuration file.
     * @param[in] log_file_path Pathname for the application log file.
     * @return Populated AuthenticationServiceConfiguration and initialised logger.
     * @throws pubsub_itc_fw::ConfigurationException on any error.
     */
    static std::tuple<AuthenticationServiceConfiguration, std::unique_ptr<pubsub_itc_fw::QuillLogger>>
    load_and_init_logging(const std::string& file_path, const std::string& log_file_path);
};

} // namespace authentication_service
