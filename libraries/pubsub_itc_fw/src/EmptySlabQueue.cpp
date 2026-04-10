// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/EmptySlabQueue.hpp>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

EmptySlabQueue::EmptySlabQueue() : dummy_{}, head_(&dummy_), tail_(&dummy_) {
    dummy_.next.store(nullptr, std::memory_order_relaxed);
    dummy_.slab_id = -1;
}

void EmptySlabQueue::enqueue(EmptySlabQueueNode* node) {
    if (node == nullptr) {
        throw PreconditionAssertion("EmptySlabQueue::enqueue: node must not be nullptr", __FILE__, __LINE__);
    }

    node->next.store(nullptr, std::memory_order_relaxed);

    EmptySlabQueueNode* prev = tail_.exchange(node, std::memory_order_acq_rel);
    prev->next.store(node, std::memory_order_release);
}

DequeueResult EmptySlabQueue::try_dequeue(int& slab_id) {
    EmptySlabQueueNode* next = head_->next.load(std::memory_order_acquire);

    if (next != nullptr) {
        head_ = next;
        slab_id = next->slab_id;
        return DequeueResult::GotItem;
    }

    if (tail_.load(std::memory_order_acquire) == head_) {
        return DequeueResult::Empty;
    }

    return DequeueResult::Retry;
}

void EmptySlabQueue::reset_to_empty() {
    dummy_.next.store(nullptr, std::memory_order_relaxed);
    head_ = &dummy_;
    tail_.store(&dummy_, std::memory_order_relaxed);
}

} // namespace pubsub_itc_fw
