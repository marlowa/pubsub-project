#pragma once

#include <cstddef>
#include <cstdint>

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

/**
 * @brief A structured snapshot of behavioural information from ExpandablePoolAllocator.
 *
 * AllocatorBehaviourStatistics provides a read-only summary of how an
 * ExpandablePoolAllocator<T> has behaved over its lifetime. It is intended
 * for diagnostic, tuning, and observability purposes. The statistics are
 * collected by ExpandablePoolAllocator::get_behaviour_statistics(), which
 * constructs an instance of this class and populates all fields.
 *
 * This class does not perform any computation itself; it is a passive data
 * container. All values are captured atomically with respect to the allocator
 * at the moment get_behaviour_statistics() is called. The statistics do not
 * prevent concurrent allocation or deallocation and do not interfere with the
 * allocator’s performance characteristics.
 *
 * The statistics include:
 *
 *   - total_allocations:
 *       The total number of successful allocations served by the allocator.
 *
 *   - fast_path_allocations:
 *       The number of allocations satisfied by the first pool in the chain
 *       without requiring a chain walk or expansion.
 *
 *   - slow_path_allocations:
 *       The number of allocations that required walking the pool chain or
 *       expanding the allocator.
 *
 *   - expansion_events:
 *       The number of times the allocator has created a new pool.
 *
 *   - failed_allocations:
 *       The number of allocation attempts that failed even after attempting
 *       expansion.
 *
 *   - per_pool_allocation_counts:
 *       A sequence of counters, one per pool, indicating how many allocations
 *       each pool has served. These values are collected from the individual
 *       FixedSizeMemoryPool<T> instances.
 *
 * Behavioural counters are intended for observability rather than strict
 * correctness. In particular, per-pool counters are non-atomic and may lose
 * increments under extreme concurrency. This does not affect allocator
 * correctness and does not compromise the usefulness of the statistics.
 *
 * This class is deliberately simple and does not expose any mutating
 * operations. It is designed to be returned by value and copied freely.
 */
class AllocatorBehaviourStatistics {
  public:

    /**
     * @brief Constructs an empty statistics object.
     *
     * All counters are initialised to zero, and the per-pool allocation
     * sequence is empty. This constructor is used internally by
     * ExpandablePoolAllocator::get_behaviour_statistics() before the fields
     * are populated.
     */
    AllocatorBehaviourStatistics() = default;

    /**
     * @brief Total number of successful allocations served by the allocator.
     *
     * This counter is incremented on every successful allocation, regardless
     * of whether the allocation was served by the fast path or the slow path.
     */
    uint64_t total_allocations{0U};

    /**
     * @brief Number of allocations served by the fast path.
     *
     * The fast path corresponds to allocations satisfied by the first pool in
     * the chain without requiring a chain walk or expansion.
     */
    uint64_t fast_path_allocations{0U};

    /**
     * @brief Number of allocations that required the slow path.
     *
     * The slow path includes:
     *   - walking the pool chain to find a pool with available capacity,
     *   - expanding the allocator by creating a new pool.
     */
    uint64_t slow_path_allocations{0U};

    /**
     * @brief Number of times the allocator has expanded by creating a new pool.
     *
     * This counter is incremented each time a new FixedSizeMemoryPool<T> is
     * added to the chain.
     */
    uint64_t expansion_events{0U};

    /**
     * @brief Number of allocation attempts that failed even after expansion.
     *
     * This counter is incremented when allocate() returns nullptr after
     * attempting to expand the allocator.
     */
    uint64_t failed_allocations{0U};

    /**
     * @brief Per-pool allocation counts.
     *
     * Each entry corresponds to a pool in the chain, in order. The value
     * represents how many allocations that pool has served over its lifetime.
     *
     * These counters are collected from the individual FixedSizeMemoryPool<T>
     * instances. They are non-atomic and may lose increments under extreme
     * concurrency, which is acceptable for their intended diagnostic purpose.
     */
    struct PerPoolAllocationCounts {
        std::vector<uint64_t> counts; // TODO revise use of unsigned integers
    } per_pool_allocation_counts;
};

} // namespace pubsub_itc_fw
