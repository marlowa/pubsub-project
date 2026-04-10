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

void ExpandableSlabAllocator::drain_empty_slab_queue() {
    int slab_id = -1;
    bool keep_draining = true;

    while (keep_draining) {
        DequeueResult result = empty_slab_queue_.try_dequeue(slab_id);

        if (result == DequeueResult::GotItem) {
            if (slab_id < 0 || slab_id >= static_cast<int>(slabs_.size())) {
                continue;
            }

            if (slab_id == current_slab_id_) {
                slabs_[slab_id]->reset();
            } else {
                slabs_[slab_id].reset();
            }
        } else if (result == DequeueResult::Retry) {
            // Producer is mid-enqueue; yield and retry once.
            continue;
        } else {
            // Queue is empty. Reset it to the initial dummy state so that
            // any slab node that was consumed (and became head_) can safely
            // be re-enqueued in a future cycle without forming a self-loop.
            empty_slab_queue_.reset_to_empty();
            keep_draining = false;
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
