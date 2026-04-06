#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * SlabAllocator manages a single contiguous mmap-backed memory region from
 * which variable-sized chunks are bump-allocated by the reactor thread.
 *
 * Threading model:
 *   - allocate() must only be called from the reactor thread.
 *   - deallocate() may be called from any thread.
 *   - When outstanding_allocations_count_ transitions from 1 to 0, the calling
 *     thread enqueues this slab's node into the provided EmptySlabQueue.
 *     The reactor then reclaims or resets the slab at a safe point.
 *
 * Alignment:
 *   All allocations are aligned to alignof(std::max_align_t).
 *
 * Slab lifecycle:
 *   - Created by ExpandableSlabAllocator with a monotonic slab ID.
 *   - Allocated from until full; reactor then switches to a new slab.
 *   - Reclaimed (reset or destroyed) by the reactor when empty.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <sys/mman.h>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A single mmap-backed slab from which variable-sized chunks are bump-allocated.
 *
 * Allocation is reactor-thread-only. Deallocation is thread-safe.
 * When the last outstanding chunk is freed, the slab notifies the reactor
 * via EmptySlabQueue.
 */
class SlabAllocator {
  public:
    /**
     * @brief Destroys the slab, releasing the mmap region.
     *
     * Must only be called by the reactor after the slab has been reclaimed.
     * Outstanding allocations must be zero at this point.
     */
    ~SlabAllocator();

    /**
     * @brief Constructs a slab of the given size.
     *
     * @param[in] slab_size    Size of the slab in bytes. Must be greater than zero.
     * @param[in] slab_id      Monotonic ID assigned by ExpandableSlabAllocator.
     * @param[in] notify_queue Queue to notify when this slab becomes empty.
     */
    SlabAllocator(size_t slab_size, int slab_id, EmptySlabQueue& notify_queue);

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;

    /**
     * @brief Bump-allocates a chunk of at least size bytes from this slab.
     *
     * Aligns the allocation to alignof(std::max_align_t).
     * Must only be called from the reactor thread.
     * Increments outstanding_allocations_count_.
     *
     * @param[in] size Number of bytes to allocate. Must be greater than zero.
     * @return Pointer to the allocated chunk, or nullptr if the slab is full.
     */
    [[nodiscard]] void* allocate(size_t size);

    /**
     * @brief Frees a chunk previously allocated from this slab.
     *
     * May be called from any thread. Decrements outstanding_allocations_count_.
     * If the count transitions to zero, enqueues this slab's node into
     * notify_queue_ so the reactor can reclaim it.
     *
     * @param[in] ptr Pointer previously returned by allocate(). Must not be nullptr.
     */
    void deallocate(void* ptr);

    /**
     * @brief Returns true if ptr points into this slab's memory region.
     *
     * @param[in] ptr Pointer to test.
     */
    [[nodiscard]] bool contains(const void* ptr) const;

    /**
     * @brief Returns true if no chunks are currently outstanding.
     */
    [[nodiscard]] bool is_empty() const;

    /**
     * @brief Returns true if no more allocations can be made from this slab.
     */
    [[nodiscard]] bool is_full() const;

    /**
     * @brief Returns the monotonic slab ID assigned at construction.
     */
    [[nodiscard]] int slab_id() const;

    /**
     * @brief Resets the slab for reuse.
     *
     * Resets the bump pointer to zero. Must only be called by the reactor
     * after confirming the slab is empty.
     */
    void reset();

    /**
     * @brief Returns the total capacity of the slab in bytes.
     */
    [[nodiscard]] size_t capacity() const;

    /**
     * @brief Returns the number of bytes currently allocated from this slab.
     */
    [[nodiscard]] size_t bytes_used() const;

    /**
     * @brief Returns a pointer to the embedded queue node.
     *
     * Used by ExpandableSlabAllocator to initialise the node's slab_id.
     */
    EmptySlabQueueNode& queue_node();

  private:
    size_t slab_size_;
    int slab_id_;
    EmptySlabQueue& notify_queue_;

    void* memory_{MAP_FAILED};
    size_t bump_{0};

    std::atomic<int> outstanding_allocations_count_{0};

    EmptySlabQueueNode queue_node_;
};

} // namespace pubsub_itc_fw
