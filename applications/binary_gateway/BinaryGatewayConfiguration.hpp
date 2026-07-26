#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>

namespace binary_gateway {

/**
 * @brief Configuration for the binary gateway application.
 *
 * The binary gateway is the order gateway's peer: same sequencers, same matching
 * engine, same book -- a different client protocol. Its configuration is
 * correspondingly smaller. There is no TLS certificate pair for a FIX listener, no
 * FIX session identity, and no authentication service, because its clients speak
 * framed PDUs and identify themselves with a single Logon PDU.
 */
struct BinaryGatewayConfiguration {
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
    // port here must match binary_gateway.port in the sequencer's configuration.

    /** @brief Host address on which the gateway listens for ER PDUs from the sequencer. */
    std::string er_listen_host{"127.0.0.1"};

    /** @brief TCP port on which the gateway listens for ER PDUs from the sequencer. */
    uint16_t er_listen_port{7110};

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

    /** @brief Shared-memory path backing the cross-process CPU registry. */
    std::string cpu_registry_shm_path;

    /** @brief Path to the flock file serialising cross-process CPU registry access. */
    std::string cpu_registry_lock_file;

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

    // No wall clock here, unlike the order gateway. That one stamps SendingTime into the
    // FIX messages it builds; this gateway builds none -- orders pass through as the
    // client encoded them, and the sequencer stamps the time that matters.
};

} // namespaces
