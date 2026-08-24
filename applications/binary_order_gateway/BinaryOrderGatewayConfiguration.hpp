#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

namespace binary_order_gateway {

/**
 * @brief Configuration for the binary gateway application.
 *
 * The binary order gateway is the FIX order gateway's peer: same sequencers, same matching
 * engine, same book, same authentication service -- a different client protocol. Its
 * configuration is correspondingly smaller: no TLS certificate pair for a FIX listener
 * and no FIX session identity, because its clients speak framed PDUs. What it does
 * share is the credential check, since an order-entry port is an order-entry port.
 */
struct BinaryOrderGatewayConfiguration {
    /**
     * @brief Which instance of this gateway protocol this process is, numbered from 1.
     *
     * Stamped onto every order envelope beside the protocol id. A protocol may run as
     * several instances for availability, and a session connection id is only unique
     * within one process, so it is the triple (protocol, instance, connection) that
     * identifies a session venue-wide and lets the sequencer route the execution report
     * back to the process the order arrived on.
     *
     * Must match a [[gateway]] entry in the sequencer's configuration; the sequencer
     * warns if it sees a pair it has no endpoint for, because it cannot deliver reports
     * to a gateway it was never told about.
     */
    int16_t instance_id{1};

    // Inbound client listener

    /** @brief Host address on which the gateway listens for binary client connections. */
    std::string listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for binary client connections. */
    uint16_t listen_port{9890};

    // Sequencer outbound connections

    /**
     * @brief Enable HA dual-publish to a secondary sequencer. When false, only the
     * primary sequencer is connected.
     */
    bool ha_enabled{false};

    /** @brief Host address of the primary sequencer. */
    std::string sequencer_primary_host{"127.0.0.1"};

    /** @brief TCP port of the primary sequencer. */
    uint16_t sequencer_primary_port{7001};

    /** @brief Host address of the secondary (follower) sequencer. Only used when ha_enabled=true. */
    std::string sequencer_secondary_host{"127.0.0.1"};

    /** @brief TCP port of the secondary (follower) sequencer. Only used when ha_enabled=true. */
    uint16_t sequencer_secondary_port{7002};

    // Inbound ExecutionReport listener
    //
    // The sequencer dials the gateway on this endpoint and pushes ERs over it, so the
    // port here must match binary_order_gateway.port in the sequencer's configuration.

    /** @brief Host address on which the gateway listens for ER PDUs from the sequencer. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for ER PDUs from the sequencer. */
    uint16_t er_listen_port{7110};

    // Authentication service
    //
    // Clients authenticate by SCRAM-SHA-256 exactly as they do on the FIX gateway, so this
    // gateway dials the same service. An order-entry port that anyone reachable could trade
    // through is not worth demonstrating, whatever the transport carrying the orders.

    /** @brief Host address of the primary authentication service. */
    std::string authentication_service_host{"127.0.0.1"};

    /** @brief TCP port of the primary authentication service. */
    uint16_t authentication_service_port{7070};

    /** @brief Host of the secondary authentication service. Only used when ha_enabled. */
    std::string authentication_service_secondary_host{"127.0.0.1"};

    /** @brief Port of the secondary authentication service. Only used when ha_enabled. */
    uint16_t authentication_service_secondary_port{7071};

    /**
     * @brief This gateway's own identity, checked against a client's TargetCompID.
     *
     * A client that names a different venue is refused, so connecting to the wrong
     * endpoint is a logon failure with a reason rather than orders going somewhere
     * unintended.
     */
    std::string sender_comp_id{"BINARY-GATEWAY"};

    /** @brief How long a SCRAM exchange may take before the logon is abandoned. */
    std::chrono::seconds scram_auth_timeout{10};

    // Cancel-on-disconnect

    /**
     * @brief Whether a lost session's resting orders are cancelled at all.
     *
     * Same contract as the FIX order gateway's: cancelling is the default because an
     * unmanaged book behind a session nobody is watching is the worse failure.
     */
    bool cancel_on_disconnect_enabled{true};

    /**
     * @brief How long a dropped session's orders rest before being cancelled.
     *
     * Set comfortably longer than a client takes to notice a dead gateway and reconnect to
     * its backup, so a gateway failure becomes a reconnect rather than a mass
     * cancellation. If the same comp id logs on again inside the window its orders are
     * reclaimed and nothing is cancelled. Zero cancels immediately.
     *
     * The binary protocol has no equivalent of a clean FIX Logout, so unlike the FIX
     * gateway every disconnect here takes the grace period.
     */
    std::chrono::seconds cancel_on_disconnect_grace_period{30};

    /** @brief Minimum severity written to the application log file. */
    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Minimum severity written to syslog. */
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Info};

    /** @brief Rolling parameters for the applog. */
    pubsub_itc_fw::RollingLogfileConfiguration rolling_logfile_configuration;

    /** @brief Enable CPU core pinning for registered application threads. */
    bool cpu_pinning_enabled{false};

    /** @brief Exclude CPU 0 from pinning candidates. */
    bool cpu_pinning_reserve_cpu0{false};

    /** @brief Path to the shared CPU registry file, under the deployment's run
     *  directory. Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_registry_shm_path;

    /** @brief Path to the flock file serialising cross-process CPU registry access. */
    std::string cpu_registry_lock_file;

    /** The machine-wide CPU layout file written by deploy.py, and this
     *  component's key within it (e.g. "sequencer_secondary" -- the instance,
     *  not the binary, since a primary and its secondary are placed separately).
     *  Cores are allocated at deploy time, not negotiated at run time.
     *  Mandatory whenever cpu_pinning_enabled is true. */
    std::string cpu_layout_file;
    std::string cpu_layout_component;

    /** @brief How often to warn while an outbound connection is retrying. */
    std::chrono::minutes connect_retry_warning_interval{15};

    /** @brief Objects per slab in the reactor command queue pool. */
    int32_t command_queue_pool_objects_per_slab{4096};

    /** @brief Initial number of slabs in the reactor command queue pool. */
    int32_t command_queue_pool_initial_slabs{1};

    /** @brief Objects per slab in the event queue pool. */
    int32_t event_queue_pool_objects_per_slab{80000};

    /** @brief Initial number of slabs in the event queue pool. */
    int32_t event_queue_pool_initial_slabs{1};

    /** @brief Objects per pool in the open-order pool, which backs cancel-on-disconnect. */
    int32_t open_order_pool_objects_per_pool{4096};

    /** @brief Initial number of pools in the open-order pool. */
    int32_t open_order_pool_initial_pools{1};

    // No wall clock here, unlike the FIX order gateway. That one stamps SendingTime into the
    // FIX messages it builds; this gateway builds none -- orders pass through as the
    // client encoded them, and the sequencer stamps the time that matters.

    /**
     * @brief This process's Prometheus scrape endpoint; see docs/operations/metrics.md.
     *
     * Copied into ReactorConfiguration, which is where the Reactor reads it from.
     */
    pubsub_itc_fw::MetricsConfiguration metrics_configuration;

    /**
     * @brief Clock used to stamp order ingress and to close the round-trip measurement.
     *
     * This gateway needs no wall time for the protocol itself -- unlike the FIX one it
     * builds no SendingTime -- so this exists only for the metric. It is nonetheless the
     * same injectable WallClock the FIX gateway holds, and deliberately not a direct call
     * to std::chrono::system_clock: the two gateways' round trips are meant to be compared,
     * so they must be measured with the same kind of clock, and a test needs to be able to
     * supply a deterministic one here exactly as it can there.
     *
     * Defaults to SystemWallClock (real UTC wall time).
     */
    std::shared_ptr<pubsub_itc_fw::WallClock> wall_clock{std::make_shared<pubsub_itc_fw::SystemWallClock>()};

    /**
     * @brief Bucket bounds in nanoseconds for order_round_trip_nanoseconds, ascending.
     *
     * Configured rather than fixed in code because the right bounds depend on the machine
     * and the offered load, and a histogram whose buckets do not bracket the latencies
     * actually being served reports every observation as one bound.
     *
     * Empty when metrics are disabled, in which case nothing registers and it is unused.
     * Must match the FIX gateway's exactly -- see GatewayMetrics.hpp.
     */
    std::vector<double> order_round_trip_buckets;
};

} // namespaces
