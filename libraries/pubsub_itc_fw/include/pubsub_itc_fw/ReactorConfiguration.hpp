#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
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
 * HA topology (primary/secondary addresses, heartbeat intervals, instance IDs)
 * is held separately by each application. The Reactor has no knowledge of HA
 * concerns.
 */
struct ReactorConfiguration {
    ReactorConfiguration() {
        // Set defaults appropriate for the reactor's internal command queue.
        command_queue_configuration_.low_watermark = 2;
        command_queue_configuration_.high_watermark = 64;
        command_allocator_configuration_.objects_per_pool = 64;
        command_allocator_configuration_.initial_pools = 1;

        // Safe defaults for the CPU pinning fields.
        //
        // In production these are always overridden by the TOML loader via
        // get_required_except(), which enforces that every application
        // explicitly sets them.  The constructor initialises them to a
        // benign state so that code paths that construct ReactorConfiguration
        // directly (unit tests, benchmarks) do not trigger undefined behaviour
        // from uninitialised booleans.
        cpu_pinning_enabled = false;
        cpu_pinning_reserve_cpu0 = false;
        // cpu_registry_lock_file is std::string -- default-constructed to "".
        // Reactor::pin_registered_threads() skips pinning when it is empty.
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
     * @brief Interval between automatic retries of a failed outbound connect attempt.
     *
     * When a non-blocking connect fails (e.g. because the remote process has not
     * started yet), the reactor schedules a retry after this interval rather than
     * delivering ConnectionFailed to the application. Retries continue indefinitely
     * until the connection succeeds.
     *
     * This is a temporary workaround for the TCP rendezvous problem -- components
     * starting at different times need to find each other. The correct long-term
     * solution is WAL-based brokerless pub/sub, where publishers write to the log
     * regardless of subscriber presence and there is no connection to establish.
     * This retry mechanism should be removed when pub/sub replaces direct TCP.
     *
     * Default: 2 seconds.
     */
    std::chrono::milliseconds connect_retry_interval_{std::chrono::seconds{2}};

    /**
     * @brief How long to wait between "still not reconnected" log warnings during retry.
     *
     * After the first connect failure is logged, subsequent retry attempts are silent.
     * Once this interval elapses without a successful reconnect, a single warning is
     * logged showing the total time disconnected, then the timer resets. The cycle
     * repeats until the connection is re-established.
     *
     * Set to zero to disable the periodic reminder (all retries silent after the first).
     *
     * Default: 15 minutes.
     */
    std::chrono::milliseconds connect_retry_warning_interval_{std::chrono::minutes{15}};

    /**
     * @brief Size in bytes of each slab used by the reactor's inbound PDU slab allocator.
     *
     * The inbound allocator receives payload bytes directly from the socket into
     * slab-allocated chunks (zero-copy). This is a hard upper bound on the size of
     * any single inbound PDU payload.
     *
     * It also sets how much payload this process may receive in its lifetime, because
     * the allocator issues a slab id per slab and never reuses one: the id directory
     * holds 262,144 entries, so the budget is that many slabs' worth of bytes. At 64 KB
     * it was 16 GiB, which a gateway can consume inside one trading day.
     *
     * 256 KB rather than something larger because a slab stays mapped while even one of
     * its chunks is outstanding, so the worst-case retention -- one live chunk holding a
     * whole slab -- rises with this value. 256 KB keeps that four times lower than 1 MB
     * while giving four times the budget of 64 KB.
     *
     * Default: 262144 bytes (256 KB).
     */
    size_t inbound_slab_size{262144};

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

    // CPU pinning

    /**
     * @brief Take part in the machine's declared CPU layout.
     *
     * When true the process masks itself to the background tier at start-up and
     * the Reactor then promotes its reactor thread and registered
     * ApplicationThreads onto whatever dedicated cores the layout allocated it.
     *
     * It should be true for **every** component on a machine that has a layout,
     * not only those expecting dedicated cores. A component that pins nothing
     * still needs the background mask, because without one it is free to be
     * scheduled onto the cores other components depend on -- and a process that
     * quietly contaminates a hot-path core is exactly the fault this design
     * exists to remove. Such a component simply finds itself unadmitted in the
     * layout and stays in the background tier.
     *
     * Mandatory: must be explicitly set from the application's TOML configuration.
     */
    bool cpu_pinning_enabled;

    /**
     * @brief Exclude CPU 0 from the set of pinning candidates.
     *
     * CPU 0 is typically used by the OS for interrupt handling and housekeeping.
     * Set to true on machines where CPU 0 is not isolated (OS/interrupt use);
     * set to false on machines with properly isolated cores.
     *
     * Mandatory: must be explicitly set from the application's TOML configuration.
     */
    bool cpu_pinning_reserve_cpu0;

    /**
     * @brief Path to the shared registry file used for cross-process coordination.
     *
     * All cooperating processes on the same machine must use the same path, and
     * it belongs inside the deployment's own run directory rather than in a
     * machine-wide location such as /dev/shm: two installations on one machine
     * must not contend for a single registry, and staleness is dealt with by the
     * deployment tooling clearing the file, not by waiting for a reboot.
     *
     * Mandatory whenever cpu_pinning_enabled is true; there is deliberately no
     * default. Reactor::initialize() fails if pinning is enabled and this is empty.
     */
    std::string cpu_registry_shm_path;

    /**
     * @brief Path to the flock file used to serialise registry access.
     *
     * Same rules as cpu_registry_shm_path: all cooperating processes on the
     * machine share one path, it lives in the deployment's run directory, and it
     * is mandatory whenever cpu_pinning_enabled is true.
     */
    std::string cpu_registry_lock_file;

    /**
     * @brief Path to the machine-wide CPU layout file generated by deploy.py.
     *
     * Cores are allocated at deploy time rather than negotiated at run time, so
     * this file is the authority on which cores this process may use. One file
     * per machine, in the deployment's run directory, read by every component.
     *
     * Mandatory whenever cpu_pinning_enabled is true.
     */
    std::string cpu_layout_file;

    /**
     * @brief This component's key in the environment TOML, e.g. "sequencer_secondary".
     *
     * Identifies which entry of the layout file belongs to this process. It has
     * to be the instance name rather than the binary name, because a primary and
     * its secondary run the same binary and are ranked and placed separately.
     * deploy.py expands it into each component's configuration when it expands
     * the templates, so it is not hand-maintained.
     *
     * Mandatory whenever cpu_pinning_enabled is true.
     */
    std::string cpu_layout_component;

    /**
     * @brief This process's Prometheus scrape endpoint.
     *
     * Held here because the Reactor owns the endpoint: it is the only object with a
     * lifetime long enough for the metric handles it hands out, and it is what sequences
     * the start of the listener after the CPU layout has been applied.
     *
     * Defaults to disabled, so a component that has not yet added a [metrics] section --
     * or a test that builds a ReactorConfiguration directly -- gets no listener and no
     * collection rather than an accidental open port.
     */
    MetricsConfiguration metrics_configuration;
};

} // namespaces
