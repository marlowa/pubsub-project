#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstddef>
#include <functional>

namespace pubsub_itc_fw {

/**
 * @file AllocationGrowthReporter.hpp
 * @brief Where a growing structure reports the size it has reached.
 *
 * The framework's pool and slab allocators instrument objects with a message lifecycle.
 * Long-lived state that GROWS -- an order book, a session table, a subscription registry --
 * goes to the OS heap and is invisible to every one of them, and can become the largest
 * consumer of memory in a process while every memory instrument reports nothing. The
 * matching engine's order book reached 9.9 GB and was OOM-killed having logged no memory
 * warning at all.
 *
 * Two things report through this struct, for two kinds of container:
 *
 * - GrowthReportingAllocator, for a container that takes an allocator and whose storage is
 *   therefore not ours to see -- a std::vector, a tsl::robin_map.
 * - IncrementalRehashMap, which owns its storage outright and reports each table directly.
 *
 * It is in its own header because it belongs to neither. A structure that reports growth
 * needs this; whether it does so through an allocator is a separate question.
 */
/**
 * @brief Shared reporting state, owned by the application rather than by any allocator.
 *
 * Separate from GrowthReportingAllocator by value, and that is a correctness requirement
 * rather than a style choice. allocator_traits copies allocators freely: a std::function held
 * by value inside one would be copied on every rebind, and a count held by value would be
 * reset by one. Every copy of an allocator must report to the same place, so the state lives
 * here and allocators point at it.
 *
 * The application owns it and must outlive the containers reporting to it.
 */
struct AllocationGrowthReporter {
    /// Invoked when a single allocation is at least report_threshold_bytes.
    /// Arguments: bytes for this allocation, and the largest seen so far.
    std::function<void(size_t, size_t)> on_large_allocation;

    /// Allocations below this are not reported. Default is deliberately generous:
    /// this exists to notice a structure becoming large, not to trace ordinary work.
    size_t report_threshold_bytes{64UL * 1024 * 1024};

    /// Largest single allocation seen. Atomic because a container may be grown from a
    /// different thread than the one that reads this for a metric or a shutdown report.
    std::atomic<size_t> largest_allocation_bytes{0};
};

} // namespaces
