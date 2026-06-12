#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>

#include <quill/core/Common.h>

namespace pubsub_itc_fw {

/**
 * @brief Custom Quill FrontendOptions for pubsub_itc_fw.
 *
 * The Quill SPSC queue starts at initial_queue_capacity and doubles each time
 * the producer thread (reactor or application thread) fills it faster than the
 * Quill backend can drain it. Quill prints an INFO message for each doubling.
 *
 * Quill's built-in default is 128 KiB. That is too small for high-throughput
 * runs where a single thread can emit hundreds of thousands of Info-level log
 * records in a short burst -- for example a matching engine processing 100 K
 * orders with per-order logging enabled. In such a run the queue was observed
 * to double seven times, reaching 32 MiB before stabilising.
 *
 * Setting initial_queue_capacity to 32 MiB matches the observed worst case and
 * eliminates all reallocation. The queue_type remains UnboundedBlocking so the
 * producer blocks rather than drops messages if the queue ever fills beyond
 * unbounded_queue_max_capacity (2 GiB, unchanged from the Quill default).
 *
 * Memory note: Quill allocates the SPSC queue using a memory-mapped ring buffer.
 * 32 MiB of virtual address space is reserved per logging thread from the moment
 * of its first log call; physical pages are faulted in only as the queue fills,
 * so the actual RSS impact is proportional to peak log throughput, not to the
 * configured size.
 *
 * All fields other than initial_queue_capacity are identical to the Quill
 * defaults in quill::FrontendOptions.
 */
struct PubsubFrontendOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
    static constexpr size_t initial_queue_capacity = 32u * 1024u * 1024u; // 32 MiB
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr size_t unbounded_queue_max_capacity = 2ull * 1024u * 1024u * 1024u; // 2 GiB
    static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};

} // namespaces
