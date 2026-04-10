#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace pubsub_itc_fw {

/**
 * @brief Configuration data for an ApplicationThread.
 *
 * Holds settings that control the behaviour of an ApplicationThread instance.
 * This struct is designed to grow as development evolves — new configuration
 * items should be added here rather than scattered across constructor parameters.
 *
 * Each ApplicationThread receives its own instance of this struct at construction.
 * Threads within the same application may share the same configuration values or
 * use distinct ones depending on their roles.
 */
struct ApplicationThreadConfig {
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
     * current slab is exhausted, so overall throughput is not limited — only the
     * size of any individual PDU frame.
     *
     * Default: 65536 bytes (64 KB).
     */
    size_t outbound_slab_size{65536};
};

} // namespace pubsub_itc_fw
