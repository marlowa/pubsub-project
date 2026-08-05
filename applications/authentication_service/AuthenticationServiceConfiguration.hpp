#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
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

    /** The machine-wide CPU layout file written by deploy.py, and this
     *  component's key within it (e.g. "sequencer_secondary" -- the instance,
     *  not the binary, since a primary and its secondary are placed separately).
     *  Cores are allocated at deploy time, not negotiated at run time.
     *  Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_layout_file;
    std::string cpu_layout_component;

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

    /**
     * @brief Per-comp_id session policy: what a gateway does with this member's resting
     *        orders when its connection goes away, and which gateway instances the member
     *        may log on to in the first place.
     *
     * Held apart from ScramCredential deliberately. That struct lives in the scram_crypto
     * library and is exactly the RFC 5802 key material; these are venue policy that happens
     * to be provisioned alongside a credential, and putting them there would make a crypto
     * type depend on trading semantics.
     *
     * Every member is optional and means "this comp id said nothing". For
     * cancel-on-disconnect that means the gateway's own default applies, which is not the
     * same as false or zero: an operator who raises the venue-wide window should not have
     * to revisit every member. For the gateway instances it means the member is not pinned
     * and may log on to any instance, which is not the same as any instance number, there
     * being no instance 0. Silence has to stay distinguishable from a deliberate value all
     * the way from the database column to the gateway.
     *
     * The instances name an instance of whichever order-entry protocol the member speaks,
     * not a protocol: this service is protocol-agnostic and must stay so.
     */
    struct SessionPolicy {
        std::optional<bool> cancel_on_disconnect_enabled;
        std::optional<int32_t> cancel_on_disconnect_grace_period_seconds;
        std::optional<int16_t> primary_gateway_instance;
        std::optional<int16_t> backup_gateway_instance;
    };

    /** @brief Per-comp_id session policy; absent entry means the gateway's defaults apply. */
    std::unordered_map<std::string, SessionPolicy> session_policies;

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/design/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;
};

} // namespaces
