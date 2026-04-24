#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief Configuration data for the Reactor.
 *
 * This struct holds settings that control the core behaviour of the Reactor:
 * the epoll event loop, thread and socket inactivity timeouts, the command
 * queue and its allocator, TCP connect timeout, and inbound PDU slab sizing.
 *
 * HA topology (primary/secondary addresses, heartbeat intervals, arbitration
 * timeouts, instance IDs) is held separately in HAConfiguration. The Reactor
 * has no knowledge of HA concerns.
 */
struct ReactorConfiguration {
    ReactorConfiguration() {
        // Set defaults appropriate for the reactor's internal command queue.
        command_queue_configuration_.low_watermark = 2;
        command_queue_configuration_.high_watermark = 64;
        command_allocator_configuration_.objects_per_pool = 64;
        command_allocator_configuration_.initial_pools = 1;
    }

    /**
     * @brief Maximum number of epoll events to process in a single epoll_wait call.
     *
     * Limits the number of events handled per reactor loop iteration. A higher
     * value increases throughput under load but may increase latency for
     * lower-priority events.
     *
     * Default: 64.
     */
    size_t max_events_per_loop{64};

    /**
     * @brief Interval between housekeeping sweeps.
     *
     * Controls how often the reactor checks for inactive threads, idle sockets,
     * timed-out connect attempts, and pending shutdown. A shorter interval
     * reduces detection latency at the cost of slightly more CPU overhead.
     *
     * Default: 1 second.
     */
    std::chrono::milliseconds inactivity_check_interval_{std::chrono::seconds{1}};

    /**
     * @brief Maximum allowed inactivity interval for inter-thread communication.
     *
     * If an ApplicationThread has not processed any ITC messages within this
     * interval it is considered stuck. The reactor logs a warning and may
     * initiate shutdown depending on policy.
     *
     * Default: 60 seconds.
     */
    std::chrono::milliseconds itc_maximum_inactivity_interval_{std::chrono::seconds{60}};

    /**
     * @brief Maximum allowed inactivity interval for sockets.
     *
     * If no data has been received on an inbound connection within this interval
     * the reactor tears it down and delivers ConnectionLost to the application
     * thread.
     *
     * Default: 60 seconds.
     */
    std::chrono::milliseconds socket_maximum_inactivity_interval_{std::chrono::seconds{60}};

    /**
     * This is the maximum time that the reactor expects an ApplicationThread to take to process the init event.
     * If that time is exceeded then the application is deemed to be unresponsive and the reactor shuts down.
     */
    std::chrono::milliseconds init_phase_timeout_{std::chrono::seconds{10}};

    /**
     * @brief Maximum time allowed for a graceful shutdown to complete.
     *
     * After shutdown() is called the reactor waits up to this interval for
     * all ApplicationThreads to reach the Terminated state before forcing
     * them down.
     *
     * Default: 1 second.
     */
    std::chrono::milliseconds shutdown_timeout_{std::chrono::seconds{1}};

    /**
     * @brief Maximum time to wait for a non-blocking TCP connect() to complete.
     *
     * If finish_connect() has not succeeded within this interval after connect()
     * was called, the reactor tears down the connection attempt and delivers a
     * ConnectionFailed event to the requesting ApplicationThread.
     *
     * Default: 5 seconds -- appropriate for LAN connections. Increase for WAN.
     */
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{5}};

    /**
     * @brief Size in bytes of each slab used by the reactor's inbound PDU slab allocator.
     *
     * The inbound allocator receives payload bytes directly from the socket into
     * slab-allocated chunks (zero-copy). This is a hard upper bound on the size of
     * any single inbound PDU payload.
     *
     * Default: 65536 bytes (64 KB).
     */
    size_t inbound_slab_size{65536};

    /**
     * @brief Size in bytes of the SO_SNDBUF socket option applied to each
     * accepted inbound connection socket, and to each outbound connection
     * socket after connect() succeeds. A value of zero means do not set
     * the option (use the OS default).
     *
     * Setting this to a value smaller than the PDU being sent is the
     * reliable way to force partial sends and exercise the continue_send()
     * path in protocol handlers.
     *
     * Default: 0 (OS default, typically ~212 KB on Linux loopback).
     */
    int socket_send_buffer_size{0};

    /**
     * @brief Size in bytes of the SO_RCVBUF socket option applied to each
     * accepted inbound connection socket, and to each outbound connection
     * socket after connect() succeeds. A value of zero means do not set
     * the option (use the OS default).
     *
     * Default: 0 (OS default).
     */
    int socket_receive_buffer_size{0};

    /**
     * @brief Queue configuration for the reactor's internal command queue.
     *
     * ApplicationThreads enqueue ReactorControlCommands here. The defaults
     * set in the constructor are appropriate for normal operation.
     */
    QueueConfiguration command_queue_configuration_;

    /**
     * @brief Allocator configuration for the reactor's internal command queue.
     *
     * The defaults set in the constructor are appropriate for normal operation.
     */
    AllocatorConfiguration command_allocator_configuration_;
};

} // namespace pubsub_itc_fw
