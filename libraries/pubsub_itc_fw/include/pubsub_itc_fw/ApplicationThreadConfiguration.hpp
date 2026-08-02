#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

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
     * **Empty means this thread registers no metrics at all**, and its recording handles stay
     * unbound, so recording through them is a safe no-op. That is the default because two
     * threads in one process sharing an empty scope would compose the same key, and
     * registering a key twice is an error. A thread therefore opts in by being named, rather
     * than every thread in every test having to be named to avoid a collision.
     */
    std::string metrics_scope{};
};
} // namespaces
