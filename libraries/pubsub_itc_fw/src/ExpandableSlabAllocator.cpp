// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>

#include <stdexcept>
#include <tuple>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

ExpandableSlabAllocator::ExpandableSlabAllocator(size_t slab_size) : slab_size_{slab_size} {
    if (slab_size_ == 0) {
        throw PreconditionAssertion("ExpandableSlabAllocator: slab_size must be greater than zero", __FILE__, __LINE__);
    }

    append_new_slab();
}

ExpandableSlabAllocator::~ExpandableSlabAllocator() = default;

std::tuple<int, void*> ExpandableSlabAllocator::allocate(size_t size) {
    if (size == 0) {
        throw PreconditionAssertion("ExpandableSlabAllocator::allocate: size must be greater than zero", __FILE__, __LINE__);
    }

    if (size > slab_size_) {
        throw PreconditionAssertion("ExpandableSlabAllocator::allocate: size exceeds slab_size", __FILE__, __LINE__);
    }

    drain_empty_slab_queue();

    SlabAllocator* current = slabs_[current_slab_id_].get();

    void* ptr = current->allocate(size);

    if (ptr == nullptr) {
        current = append_new_slab();
        ptr = current->allocate(size);
    }

    if (ptr == nullptr) {
        throw PubSubItcException(
            "ExpandableSlabAllocator::allocate: allocation failed after chaining a new slab — "
            "this should not happen; mmap may have failed during slab construction");
    }

    return {current_slab_id_, ptr};
}

void ExpandableSlabAllocator::deallocate(int slab_id, void* ptr) {
    if (ptr == nullptr) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: ptr must not be nullptr", __FILE__, __LINE__);
    }

    if (slab_id < 0 || slab_id >= static_cast<int>(slabs_.size())) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: slab_id out of range", __FILE__, __LINE__);
    }

    SlabAllocator* slab = slabs_[slab_id].get();

    if (slab == nullptr) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: slab has already been destroyed", __FILE__, __LINE__);
    }

    slab->deallocate(ptr);
}

int ExpandableSlabAllocator::slab_count() const {
    return static_cast<int>(slabs_.size());
}

size_t ExpandableSlabAllocator::slab_size() const {
    return slab_size_;
}

void ExpandableSlabAllocator::drain_empty_slab_queue()
{
    // Collect all slab IDs from the queue before processing any of them.
    // The Vyukov MPSC queue threads pointers through the embedded node inside
    // each SlabAllocator. Destroying a slab while still traversing the queue
    // frees the node that the queue's internal head pointer may still reference,
    // causing a use-after-free. Separating collection from processing ensures
    // the queue traversal completes before any slab is reset or destroyed.
    //
    // The vector allocation here is not a hot-path concern. This function only
    // does real work when one or more slabs have become fully empty, which is an
    // infrequent event relative to the steady-state allocation rate. On the
    // common path the queue is empty and the vector is never allocated at all.
    std::vector<int> ids_to_reclaim;

    for (;;) {
        int slab_id = -1;
        DequeueResult result = empty_slab_queue_.try_dequeue(slab_id);

        if (result == DequeueResult::GotItem) {
            if (slab_id >= 0 && slab_id < static_cast<int>(slabs_.size())) {
                ids_to_reclaim.push_back(slab_id);
            }
        } else if (result == DequeueResult::Retry) {
            // Producer is mid-enqueue; yield and retry once.
            continue;
        } else {
            // Queue is empty. Reset it to the initial dummy state so that
            // any slab node that was consumed (and became head_) can safely
            // be re-enqueued in a future cycle without forming a self-loop.
            empty_slab_queue_.reset_to_empty();
            break;
        }
    }

    for (int slab_id : ids_to_reclaim) {
        if (slab_id == current_slab_id_) {
            slabs_[slab_id]->reset();
        } else {
            slabs_[slab_id].reset();
        }
    }
}

SlabAllocator* ExpandableSlabAllocator::append_new_slab() {
    int new_id = static_cast<int>(slabs_.size());
    slabs_.push_back(std::make_unique<SlabAllocator>(slab_size_, new_id, empty_slab_queue_));
    current_slab_id_ = new_id;
    return slabs_[current_slab_id_].get();
}

} // namespace pubsub_itc_fw
