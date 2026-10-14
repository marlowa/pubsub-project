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
 * Empty-slab notification, and who may touch the slab last:
 *   Each slab is in one of two states: current or not-current. Only non-current
 *   slabs are put on the EmptySlabQueue. While a slab is current the owner
 *   thread holds a direct pointer to it and resets it in place when it empties.
 *
 *   The slab keeps one count, references_: the chunks allocated and not yet
 *   freed, plus one held by the owner for as long as the slab is current. The
 *   count therefore cannot reach zero while the slab is current, and whichever
 *   party takes it to zero is the last party that will ever touch the slab:
 *
 *   - A deallocator that takes it to zero puts the slab on the queue, and
 *     touches nothing of the slab after that but its queue node.
 *   - The owner, switching away, gives up its hold with stop_being_current();
 *     if that takes the count to zero, no chunk is outstanding and the owner
 *     puts the slab on the queue itself.
 *
 *   Exactly one party puts the slab on the queue, and it is the last to touch
 *   it, so the owner can destroy the slab once the queue gives it back without
 *   any deallocator still reading it. A reclaimed slab is destroyed; its node
 *   never returns to the queue.
 *
 * Self-reclaim of the current slab:
 *   The owner thread may reset a current slab's bump pointer inline when
 *   outstanding_count() is zero, meaning only its own hold remains. This is safe
 *   because the owner is the sole allocator; no chunk is outstanding, so no
 *   deallocator is active for this slab. See ExpandableSlabAllocator::allocate().
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
     * Must only be called from the owner thread.
     * Adds one to references_.
     *
     * @param[in] size Number of bytes to allocate. Must be greater than zero.
     * @return Pointer to the allocated chunk, or nullptr if the slab is full.
     */
    [[nodiscard]] void* allocate(size_t size);

    /**
     * @brief Frees a chunk previously allocated from this slab.
     *
     * May be called from any thread. Takes one from references_. If that takes
     * it to zero, the slab is no longer current and this was its last chunk, so
     * this call puts the slab's node on notify_queue_ for the owner to reclaim,
     * and touches the slab no further.
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
     * Resets the bump pointer to zero. Used by the owner when the current slab
     * has no chunk outstanding and the owner wants to keep using the same slab
     * rather than chaining a new one. references_ is left holding the owner's
     * one count, which is all it holds when no chunk is outstanding.
     *
     * Preconditions enforced by the caller:
     *   - The slab is the current slab (is_current() returns true).
     *   - outstanding_count() is zero.
     *
     * Must only be called by the owner thread. A current slab's node has never
     * been put on the queue.
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
     * @brief Returns the number of chunks allocated and not yet freed.
     *
     * The owner's own hold on a current slab is not counted. Owner-thread
     * query, used to decide whether the current slab may be reset in place.
     */
    [[nodiscard]] int outstanding_count() const;

    /**
     * @brief Returns whether this slab is the current slab.
     */
    [[nodiscard]] bool is_current() const;

    /**
     * @brief The owner switches away from this slab, and gives up its hold on it.
     *
     * Called by ExpandableSlabAllocator::append_new_slab on the slab being
     * switched away from. From this point on, the deallocator that frees the
     * last outstanding chunk puts the slab on the queue.
     *
     * Must be called from the owner thread only, once.
     *
     * @return true if no chunk was outstanding, so that giving up the hold took
     *         references_ to zero: no deallocator will ever put the slab on the
     *         queue, and the owner must do it itself. False otherwise.
     */
    [[nodiscard]] bool stop_being_current();

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

    // The chunks allocated and not yet freed, plus one held by the owner while
    // the slab is current. See the class description for why the owner's hold
    // is counted here.
    std::atomic<int> references_{1};
    // Written only by the owner thread, and read only by it.
    bool is_current_{true};

    EmptySlabQueueNode queue_node_;
};

} // namespaces
