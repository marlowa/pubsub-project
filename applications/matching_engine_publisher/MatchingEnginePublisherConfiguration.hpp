#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>

namespace matching_engine_publisher {

/**
 * @brief Configuration for the Matching Engine Publisher (MEP).
 *
 * MEP sits between the sequencer (source of truth) and downstream topic
 * subscribers. It follows the sequencer's WAL as an external subscriber,
 * writes a local WAL, and fans records out to connected topic subscribers
 * via the topic pub/sub protocol.
 *
 * Two topics are served:
 *   "orders"             -- pdu_id 1000 (NOS) and 1001 (OCR)
 *   "execution_reports"  -- pdu_id 1002 (ER)
 *
 * HA: primary/secondary pair managed by the arbiter pool.
 * Only the leader streams topic records; the secondary replies with
 * TopicNotLeader to any TopicSubscribeRequest.
 */
struct MatchingEnginePublisherConfiguration {

    // ----------------------------------------------------------------
    // Network -- listen address used for all inbound listeners
    // ----------------------------------------------------------------

    std::string listen_host{"127.0.0.1"};

    // ----------------------------------------------------------------
    // Sequencer WAL follower connections
    //   MEP connects outbound to both sequencer instances and keeps
    //   both connections warm. Only the sequencer leader streams records.
    // ----------------------------------------------------------------

    std::string sequencer_wal_host{"127.0.0.1"};
    uint16_t    sequencer_wal_port{7030};

    std::string sequencer_wal_secondary_host{"127.0.0.1"};
    uint16_t    sequencer_wal_secondary_port{7031};

    // ----------------------------------------------------------------
    // Topic inbound listeners
    // ----------------------------------------------------------------

    uint16_t orders_listen_port{7040};
    uint16_t er_listen_port{7041};

    // ----------------------------------------------------------------
    // WAL -- MEP's own write-ahead log
    // ----------------------------------------------------------------

    std::string wal_directory{"/var/tmp/pubsub/mep_wal"};
    size_t      wal_segment_size{4 * 1024 * 1024};
    int32_t     snapshot_interval_seconds{30};

    // ----------------------------------------------------------------
    // HA -- leader-follower via arbiter pool
    // ----------------------------------------------------------------

    bool    ha_enabled{false};
    int32_t instance_id{1};

    std::string arbiter_primary_host{"127.0.0.1"};
    uint16_t    arbiter_primary_port{7200};

    std::string arbiter_secondary_host{"127.0.0.1"};
    uint16_t    arbiter_secondary_port{7201};

    int32_t arbitration_timeout_seconds{3};

    std::string peer_listen_host{"127.0.0.1"};
    uint16_t    peer_listen_port{7044};

    std::string peer_host{"127.0.0.1"};
    uint16_t    peer_port{7045};

    int32_t heartbeat_interval_seconds{5};
    int32_t heartbeat_timeout_seconds{15};
    int32_t startup_election_timeout_seconds{3};

    std::string fence_file_path{"/dev/shm/mep_fence"};

    // ----------------------------------------------------------------
    // Subscriber flow control
    // ----------------------------------------------------------------

    int64_t max_lag_records{100000};

    // ----------------------------------------------------------------
    // Logging
    // ----------------------------------------------------------------

    pubsub_itc_fw::FwLogLevel applog_level{pubsub_itc_fw::FwLogLevel::Info};
    pubsub_itc_fw::FwLogLevel syslog_level{pubsub_itc_fw::FwLogLevel::Critical};

    // ----------------------------------------------------------------
    // Reactor
    // ----------------------------------------------------------------

    bool cpu_pinning_enabled{false};
    bool cpu_pinning_reserve_cpu0{false};
    std::string cpu_registry_lock_file;
    std::chrono::milliseconds connect_retry_warning_interval{std::chrono::minutes(15)};

    // ----------------------------------------------------------------
    // Event queue pool
    // ----------------------------------------------------------------

    int32_t event_queue_pool_objects_per_slab{1024};
    int32_t event_queue_pool_initial_slabs{1};

    // ----------------------------------------------------------------
    // Command queue pool
    // ----------------------------------------------------------------

    int32_t command_queue_pool_objects_per_slab{1024};
    int32_t command_queue_pool_initial_slabs{1};
};

} // namespaces
