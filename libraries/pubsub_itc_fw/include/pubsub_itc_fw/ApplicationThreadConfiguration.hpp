#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef> // IWYU pragma: keep
#include <string>

namespace pubsub_itc_fw {
/**
 * @brief Configuration data for an ApplicationThread.
 *
 * Holds settings that control the behaviour of an ApplicationThread instance.
 * This struct is designed to grow as development evolves -- new configuration
 * items should be added here rather than scattered across constructor parameters.
 *
 * Each ApplicationThread receives its own instance of this struct at construction.
 * Threads within the same application may share the same configuration values or
 * use distinct ones depending on their roles.
 */
struct ApplicationThreadConfiguration {
    /**
     * @brief Size in bytes of each slab used by this thread's outbound PDU slab allocator.
     *
     * Each ApplicationThread owns its own ExpandableSlabAllocator for outbound PDUs.
     * The thread allocates a chunk from this allocator, encodes the PDU into it, and
     * enqueues a SendPdu command to the reactor. The reactor deallocates the chunk
     * once the send is complete.
     *
     * This value is a hard upper bound on the size of any single outbound PDU frame
     * (sizeof(PduHeader) + payload). An attempt to allocate a frame larger than this
     * value will throw PreconditionAssertion. This constraint is intentional: it
     * encourages application designers to decompose large responses into multiple
     * focused messages rather than sending unbounded lists in a single PDU, which
     * would cause latency spikes and unpredictable memory pressure.
     *
     * The allocator grows automatically by chaining new slabs of this size when the
     * current slab is exhausted, so overall throughput is not limited -- only the
     * size of any individual PDU frame.
     *
     * Default: 65536 bytes (64 KB).
     */
    size_t outbound_slab_size{65536};

    /**
     * @brief Size in bytes of the decode arena buffer owned by this thread.
     *
     * This buffer provides backing store for BumpAllocator when decoding
     * variable-length inbound PDUs. It is reserved once at construction time
     * and reused for every inbound PDU -- there is no heap allocation on the
     * message handling path.
     *
     * This value must be at least as large as the maximum arena bytes required
     * by any inbound PDU type this thread will receive, which is bounded by
     * the reactor's inbound_slab_size. The two values should be kept consistent:
     * set this to the same value as ReactorConfiguration::inbound_slab_size.
     *
     * Default: 65536 bytes (64 KB).
     */
    size_t inbound_decode_arena_size{65536};

    /**
     * @brief Scope label for this thread's metrics, or empty to record none.
     *
     * Metric keys are `<application>.<component>[.<scope>].<metricName>`. The application and
     * component come from MetricsConfiguration and identify the process; this is the third
     * token, and is what tells one thread's metrics apart from another's in the same process.
     *
     * It is deliberately not derived from the thread name. The thread name is chosen for
     * people reading logs, so renaming one would silently break every dashboard built on it;
     * it is CamelCase where label values elsewhere are lowercase; and it is not always a legal
     * token -- the topic probe names its thread "ProbeThread-<topic>", and a hyphen is not
     * permitted in a metric key. Naming the scope separately keeps the two free to differ.
     *
     * Must be a single token of [A-Za-z0-9_]; MetricKey rejects anything else when the metric
     * is registered. Prefer lowercase, matching the application and component values, e.g.
     * "sequencer_thread".
     *
     * Set it through a small named helper, not a designated initialiser -- this project
     * builds as C++17, where designated initialisers are a C++20 feature and -Werror rejects
     * them. The components that opt in follow the make_thread_config() pattern:
     *
     *     pubsub_itc_fw::ApplicationThreadConfiguration make_thread_config() {
     *         pubsub_itc_fw::ApplicationThreadConfiguration configuration;
     *         configuration.metrics_scope = "sequencer_thread";
     *         return configuration;
     *     }
     *
     * **Empty means this thread registers no metrics at all**, and its recording handles stay
     * unbound, so recording through them is a safe no-op. That is the default because two
     * threads in one process sharing an empty scope would compose the same key, and
     * registering a key twice is an error. A thread therefore opts in by being named, rather
     * than every thread in every test having to be named to avoid a collision.
     */
    std::string metrics_scope{};

    /**
     * @brief How long to keep polling an empty queue before blocking, or zero not to poll.
     *
     * When its queue empties, a thread blocks in `epoll_wait` until a producer signals it.
     * Blocking deschedules the thread, and if nothing else is runnable on its core -- which
     * is guaranteed for a pinned hot-path thread, since nothing else is scheduled there --
     * the core goes idle. Waking it costs whatever the deepest idle state the core reached
     * costs to leave, which on a machine with the usual defaults can be around a millisecond.
     * The wakeup also costs a system call and a trip through the scheduler even when the core
     * stayed awake.
     *
     * Setting this makes the thread poll the queue for up to this long before it blocks. Work
     * arriving inside that window is picked up with no system call, no scheduler involvement,
     * and no core to wake.
     *
     * **It only bridges gaps shorter than itself, so choose it from the traffic rather than
     * from taste.** A thread receiving 150 messages a second sees mean gaps near 6.7ms, and a
     * 20us poll bridges none of them: the core sleeps exactly as before. Bridging those gaps
     * needs a poll of milliseconds, which is a busy-wait holding a core permanently. The
     * benefit therefore grows with message rate, which is the opposite of where the idle-state
     * penalty is worst, and it is not a substitute for configuring the machine.
     *
     * **Default zero, meaning block immediately, because polling is not free.** A thread
     * polling for 100us every time its queue empties consumes a core for that whole period,
     * and a process with several such threads consumes several. Threads that are not on a
     * latency-critical path, and every thread in a test, should leave this alone.
     *
     * **Values much above 50us buy less than they appear to.** The polling uses
     * BackoffWithYield, whose first tier issues the processor's spin-wait hint and covers
     * roughly 48us before it starts yielding to the scheduler and then sleeping. Yielding and
     * sleeping are themselves context switches, which is the cost this setting exists to
     * avoid, so beyond that first tier the benefit tails off. That degradation is deliberate:
     * it stops a large value from holding a core for milliseconds. Treat about 50us as the
     * range over which this does what it says.
     *
     * Set it through the same named-helper pattern metrics_scope describes:
     *
     *     configuration.spin_before_block = std::chrono::microseconds{40};
     */
    std::chrono::microseconds spin_before_block{0};
};
} // namespaces
