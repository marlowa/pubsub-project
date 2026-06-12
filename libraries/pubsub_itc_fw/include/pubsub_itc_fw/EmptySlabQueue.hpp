#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * EmptySlabQueue is a bespoke, allocator-free, intrusive Vyukov-style MPSC queue
 * used exclusively to notify the reactor that a slab has become empty.
 *
 * One EmptySlabQueueNode is embedded in each slab. When a thread decrements a
 * slab's outstanding_allocations_count to zero, it enqueues that slab's node.
 * The reactor drains this queue before performing new allocations.
 *
 * This queue has no dependency on any pool allocator or higher-level structure.
 * It uses no dynamic allocation. Capacity is bounded by the number of live slabs.
 *
 * Threading model:
 *   Producers: any application thread (enqueue only)
 *   Consumer:  reactor thread only (dequeue only)
 */

#include <atomic>
#include <cstddef>

namespace pubsub_itc_fw {

/**
 * @brief A node embedded in each slab for use with EmptySlabQueue.
 *
 * Each slab owns exactly one node. The node is reused each time the slab
 * becomes empty. No dynamic allocation is performed.
 */
struct EmptySlabQueueNode {
    std::atomic<EmptySlabQueueNode*> next{nullptr};
    int slab_id{-1};
};

/**
 * @brief Dequeue result returned by EmptySlabQueue::try_dequeue().
 */
enum class DequeueResult {
    GotItem, ///< A slab ID was returned.
    Empty,   ///< Queue is empty.
    Retry    ///< Producer is mid-enqueue; caller should retry.
};

/**
 * @brief Lock-free, allocator-free, intrusive MPSC queue for empty-slab notifications.
 *
 * Producers enqueue using a single atomic exchange (Vyukov MPSC pattern).
 * The reactor is the sole consumer and drains the queue before allocating.
 *
 * Correctness invariants:
 *   - A slab's node is enqueued at most once per empty episode.
 *   - Only the thread that decrements outstanding_allocations_count to zero enqueues.
 *   - Only the reactor advances head_.
 *   - No allocation, no locks, no ABA hazard.
 */
class EmptySlabQueue {
  public:
    ~EmptySlabQueue() = default;
    EmptySlabQueue();

    EmptySlabQueue(const EmptySlabQueue&) = delete;
    EmptySlabQueue& operator=(const EmptySlabQueue&) = delete;

    /**
     * @brief Enqueues a slab ID notification.
     *
     * Called by the thread that observes the outstanding_allocations_count
     * transition from 1 to 0. Must not be called more than once per empty
     * episode for a given slab.
     *
     * @param[in] node Pointer to the slab's embedded EmptySlabQueueNode.
     *                 Must not be nullptr.
     */
    void enqueue(EmptySlabQueueNode* node);

    /**
     * @brief Attempts to dequeue one slab ID notification.
     *
     * Called only by the reactor thread. Returns GotItem and sets slab_id
     * if an item was available, Empty if the queue is empty, or Retry if a
     * producer is mid-enqueue.
     *
     * @param[out] slab_id Set to the dequeued slab ID on GotItem.
     * @return DequeueResult indicating the outcome.
     */
    [[nodiscard]] DequeueResult try_dequeue(int& slab_id);

    /**
     * @brief Returns the current head pointer.
     *
     * Diagnostic-only accessor for the consumer thread. The returned pointer
     * is the consumer's view of the head; producers do not read or write it.
     * Reads of head_ must therefore be from the consumer (reactor) thread.
     */
    [[nodiscard]] const EmptySlabQueueNode* peek_head() const {
        return head_;
    }

    /**
     * @brief Returns the current value of head_->next.
     *
     * Diagnostic-only accessor. Used by drain_empty_slab_queue to inspect why
     * the queue appears not to be making progress.
     */
    [[nodiscard]] const EmptySlabQueueNode* peek_head_next() const {
        return head_ != nullptr ? head_->next.load(std::memory_order_acquire) : nullptr;
    }

    /**
     * @brief Returns the current tail pointer.
     *
     * Diagnostic-only accessor. Used by drain_empty_slab_queue to inspect why
     * the queue appears not to be making progress.
     */
    [[nodiscard]] const EmptySlabQueueNode* peek_tail() const {
        return tail_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns the address of the dummy_ sentinel.
     *
     * Diagnostic-only accessor. Useful for tests that want to detect when
     * head_ or tail_ has been reset back to the dummy sentinel.
     */
    [[nodiscard]] const EmptySlabQueueNode* peek_dummy() const {
        return &dummy_;
    }

  private:
    EmptySlabQueueNode dummy_;
    EmptySlabQueueNode* head_;
    std::atomic<EmptySlabQueueNode*> tail_;
};

} // namespaces
