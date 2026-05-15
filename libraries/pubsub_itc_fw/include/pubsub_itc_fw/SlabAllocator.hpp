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
 *
 * Empty-slab notification (decoupled from current-slab status):
 *   Each slab is in one of two states: current or not-current. Only non-current
 *   slabs participate in the EmptySlabQueue. While a slab is current the owner
 *   thread holds a direct pointer to it; there is no benefit to notifying
 *   through the queue, and doing so would risk re-enqueueing the same node
 *   while a previous notification is still being processed.
 *
 *   - At construction the slab is current. Deallocators never enqueue.
 *   - The owner clears is_current_ when it switches away from this slab
 *     (see ExpandableSlabAllocator::append_new_slab). From that moment on,
 *     the next deallocator that drives the outstanding count to zero will
 *     atomically claim and perform the single one-shot enqueue (is_enqueued_
 *     CAS). If the count is already zero at the moment the owner clears
 *     is_current_, the owner performs the enqueue itself.
 *   - A reclaimed slab is destroyed; its node never returns to the queue.
 *
 * Self-reclaim of the current slab:
 *   The owner thread may also reset a current slab's bump pointer inline if
 *   it observes outstanding_allocations_count_ == 0. This is safe because the
 *   owner is the sole allocator; a zero count means no deallocator is active
 *   for this slab. See ExpandableSlabAllocator::allocate().
 *
 * Alignment:
 *   All allocations are aligned to alignof(std::max_align_t).
 *
 * Slab lifecycle:
 *   - Created by ExpandableSlabAllocator with a monotonic slab ID.
 *   - Allocated from until full or until the owner switches to a new slab.
 *   - If switched away from: reclaimed via EmptySlabQueue and destroyed by
 *     the reactor.
 *   - If kept current: bump pointer may be reset inline when outstanding
 *     count drops to zero.
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
     * @brief Resets the slab for reuse in place.
     *
     * Resets the bump pointer to zero and zeroes the outstanding allocation
     * count. Used by the reactor when a current slab has been fully drained
     * (outstanding count is zero) and the reactor wants to keep using the
     * same slab for further allocations rather than chaining a new one.
     *
     * Preconditions enforced by the caller:
     *   - The slab is the current slab (is_current() returns true).
     *   - outstanding_count() is zero.
     *
     * Must only be called by the reactor thread. The is_enqueued_ flag is
     * left untouched: a current slab's node has never been enqueued, so
     * the flag is already false.
     *
     * Does NOT touch queue_node_.next. That field is structural state owned
     * by EmptySlabQueue. After dequeue, head_ points to this node as the
     * new dummy; zeroing next would sever any link the queue has already
     * established through it.
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
     * @brief Returns the current outstanding allocation count.
     *
     * Reactor-thread query used to decide whether the slab is safe to
     * self-reclaim inline (bump pointer reset) or to enqueue at switch-away
     * time.
     */
    [[nodiscard]] int outstanding_count() const;

    /**
     * @brief Returns whether this slab is the current slab.
     */
    [[nodiscard]] bool is_current() const;

    /**
     * @brief Clears the is_current flag.
     *
     * Called by ExpandableSlabAllocator::append_new_slab on the slab being
     * switched away from. From this point on, the next deallocator that
     * drives the count to zero will enqueue the slab's notification node.
     * Must be called from the reactor (owner) thread only.
     */
    void clear_is_current();

    /**
     * @brief Attempts to claim the one-shot enqueue right.
     *
     * Atomically CAS is_enqueued_ from false to true.
     *
     * Used by:
     *   - SlabAllocator::deallocate when the count transitions to zero AND
     *     is_current is false, to ensure exactly one enqueue happens.
     *   - ExpandableSlabAllocator::append_new_slab after clearing is_current,
     *     if the count was already zero at that moment.
     *
     * @return true if this call won the claim; false if some other code path
     *         had already claimed it.
     */
    [[nodiscard]] bool try_claim_enqueue();

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
    std::atomic<bool> is_current_{true};
    std::atomic<bool> is_enqueued_{false};

    EmptySlabQueueNode queue_node_;
};

} // namespace pubsub_itc_fw
