#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>

#include <scram_crypto/ScramCrypto.hpp>

namespace authentication_service {

/**
 * @brief Configuration for the authentication service application.
 *
 * The authentication service listens for inbound PDU connections from gateways
 * and handles the SCRAM-SHA-256 four-message exchange for each logon attempt.
 * Gateway-to-authentication-service communication is internal to the project and
 * uses plain TCP with PDU framing; TLS is not used on this path.
 */
struct AuthenticationServiceConfiguration {
    // ----------------------------------------------------------------
    // Network
    // ----------------------------------------------------------------

    /** @brief Host address on which the service listens for inbound connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the service listens for inbound connections. */
    uint16_t listen_port{7070};

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

    // ----------------------------------------------------------------
    // Credentials
    // ----------------------------------------------------------------

    /** @brief Path to the TOML file containing per-comp_id SCRAM-SHA-256 credentials. */
    std::string credentials_file;

    /** @brief Per-comp_id SCRAM credentials populated from credentials_file at startup. */
    std::unordered_map<std::string, scram_crypto::ScramCredential> credentials;
};

} // namespace authentication_service
