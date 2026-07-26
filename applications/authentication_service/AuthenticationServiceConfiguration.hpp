#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>

#include <scram_crypto/ScramCrypto.hpp>

namespace authentication_service {

/**
 * @brief Configuration for the authentication service application.
 *
 * The authentication service exposes two listeners:
 *   - PDU listener (plain TCP): for gateway SCRAM-SHA-256 exchanges.
 *   - TLS admin listener: for credential management (SetCredential). The
 *     plaintext password is protected by TLS in transit; the service derives
 *     and stores only the SCRAM-SHA-256 values.
 */
struct AuthenticationServiceConfiguration {
    // Network -- PDU listener (gateway authentication exchanges)

    /** @brief Host address on which the service listens for inbound connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the service listens for inbound PDU connections. */
    uint16_t listen_port{7070};

    // Network -- TLS admin listener (credential management)

    /** @brief TCP port on which the service listens for TLS admin connections. */
    uint16_t admin_listen_port{7072};

    /** @brief Path to the PEM-encoded server certificate for the admin TLS listener. */
    std::string admin_tls_certificate_path;

    /** @brief Path to the PEM-encoded private key for the admin TLS listener. */
    std::string admin_tls_private_key_path;

    /** @brief Path to the PEM-encoded CA certificate used to verify admin client
     *  certificates. Empty string disables client certificate verification. */
    std::string admin_tls_ca_path;

    /** @brief If true, admin clients must present a valid certificate signed by the CA.
     *  Ignored when admin_tls_ca_path is empty. */
    bool admin_tls_require_client_certificate{false};

    // Logging

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the application log. */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;

    // Reactor

    /** @brief Enable CPU core pinning for registered application threads.
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_enabled;

    /** @brief Exclude CPU 0 from pinning candidates (for machines without isolated cores).
     *  Mandatory: must be set explicitly in the TOML configuration file. */
    bool cpu_pinning_reserve_cpu0;

    /** @brief Path to the shared CPU registry file, and to the flock file that serialises
     *  access to it. Both live under the deployment's run directory so that two
     *  installations on one machine cannot contend for a single registry.
     *  Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_registry_shm_path;
    std::string cpu_registry_lock_file;

    /** @brief How long to wait between "still disconnected" log warnings during outbound retry. */
    std::chrono::milliseconds connect_retry_warning_interval;

    // Event queue pool  (ApplicationThread inbound EventMessage queue)

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t event_queue_pool_objects_per_slab{64};

    /** @brief Number of event queue pool slabs pre-allocated at startup. */
    int32_t event_queue_pool_initial_slabs{1};

    // Command queue pool  (Reactor ReactorControlCommand outbound queue)

    /** @brief Number of objects in each fixed-size memory pool slab. */
    int32_t command_queue_pool_objects_per_slab{64};

    /** @brief Number of command queue pool slabs pre-allocated at startup. */
    int32_t command_queue_pool_initial_slabs{1};

    // Credentials

    /** @brief Path to the TOML file containing per-comp_id SCRAM-SHA-256 credentials. */
    std::string credentials_file;

    /** @brief Per-comp_id SCRAM credentials populated from credentials_file at startup. */
    std::unordered_map<std::string, scram_crypto::ScramCredential> credentials;
};

} // namespaces
