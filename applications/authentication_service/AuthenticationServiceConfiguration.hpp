#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>

namespace authentication_service {

/**
 * @brief Configuration for the authentication service application.
 *
 * The authentication service listens for inbound TLS connections from gateways
 * and handles the SCRAM-SHA-256 four-message exchange for each logon attempt.
 * All fields have sensible defaults for local development. Production deployments
 * must provide real TLS certificate paths and credentials.
 */
struct AuthenticationServiceConfiguration {
    // ----------------------------------------------------------------
    // Network
    // ----------------------------------------------------------------

    /** @brief Host address on which the service listens for inbound TLS connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the service listens for inbound TLS connections. */
    uint16_t listen_port{7070};

    /** @brief Size in bytes of the per-connection raw receive buffer. */
    int64_t raw_buffer_capacity{65536};

    // ----------------------------------------------------------------
    // TLS
    // ----------------------------------------------------------------

    /** @brief Path to the server certificate PEM file. */
    std::string certificate_path;

    /** @brief Path to the server private key PEM file. */
    std::string private_key_path;

    /** @brief Path to the CA certificate PEM file used for client certificate
     *         verification. Empty means client certificates are not verified. */
    std::string ca_path;

    /** @brief If true, require connecting clients to present a certificate
     *         signed by the CA at ca_path. Ignored when ca_path is empty. */
    bool require_client_certificate{false};

    // ----------------------------------------------------------------
    // Logging
    // ----------------------------------------------------------------

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the application log. */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;

    // ----------------------------------------------------------------
    // Reactor
    // ----------------------------------------------------------------

    /** @brief Enable CPU core pinning for registered application threads.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_enabled;

    /** @brief Exclude CPU 0 from pinning candidates (for machines without isolated cores).
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_dev_mode;

    /** @brief Path to the flock file used to serialise cross-process CPU registry access.
     *  Prefer /dev/shm/ so the file is cleared on reboot.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    std::string cpu_registry_lock_file;

    // ----------------------------------------------------------------
    // Event queue pool  (ApplicationThread inbound EventMessage queue)
    // ----------------------------------------------------------------

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t event_queue_pool_objects_per_slab{64};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // ----------------------------------------------------------------
    // Command queue pool  (Reactor ReactorControlCommand outbound queue)
    // ----------------------------------------------------------------

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t command_queue_pool_objects_per_slab{64};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};
};

} // namespace authentication_service
