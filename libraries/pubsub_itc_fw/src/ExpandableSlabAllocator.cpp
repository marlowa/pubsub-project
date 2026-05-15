// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>

#include <stdexcept>
#include <string>
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

    // Opportunistic self-reclaim of the current slab. The current slab is
    // never reclaimed via the empty-slab queue (only non-current slabs are),
    // so the bump pointer would otherwise grow until the slab fills up. If
    // the application thread has drained all outstanding allocations on the
    // current slab, we can safely reset the bump pointer here and reuse the
    // existing slab memory.
    //
    // Safety: this is the only allocator thread; outstanding_count() == 0
    // implies no deallocator can be active for this slab, so its state is
    // stable.
    if (current->outstanding_count() == 0) {
        current->reset();
    }

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

    // Safety tripwire. The drain loop is bounded by the number of live slabs;
    // exceeding any sane multiple of that is a strong signal of a queue-state
    // corruption (e.g. a node self-loop). Rather than spin until OOM, fail
    // fast with a descriptive exception so the reactor terminates cleanly and
    // the operator can see what went wrong.
    //
    // The threshold is deliberately generous: legitimate drains terminate in
    // O(live slabs), so even a pathologically large pool of thousands of
    // slabs would finish in thousands of iterations, not 100,000.
    int64_t loop_iterations  = 0;
    int64_t retry_count      = 0;
    int64_t got_item_count   = 0;
    int     last_got_slab_id = -1;
    int64_t same_id_repeats  = 0;
    static constexpr int64_t tripwire_iterations = 100'000;

    for (;;) {
        ++loop_iterations;
        if (loop_iterations > tripwire_iterations) {
            throw PubSubItcException(
                "ExpandableSlabAllocator::drain_empty_slab_queue: "
                "tripwire exceeded; queue is not making progress. "
                "Likely a corrupted lock-free queue state (self-loop or stuck producer). "
                "Diagnostics: got_item=" + std::to_string(got_item_count)
                + " retry=" + std::to_string(retry_count)
                + " last_slab_id=" + std::to_string(last_got_slab_id)
                + " same_id_repeats=" + std::to_string(same_id_repeats)
                + " live_slabs=" + std::to_string(slabs_.size())
                + " ids_to_reclaim.size=" + std::to_string(ids_to_reclaim.size()));
        }

        int slab_id = -1;
        DequeueResult result = empty_slab_queue_.try_dequeue(slab_id);

        if (result == DequeueResult::GotItem) {
            ++got_item_count;
            if (slab_id == last_got_slab_id) {
                ++same_id_repeats;
            } else {
                last_got_slab_id = slab_id;
                same_id_repeats  = 0;
            }
            if (slab_id >= 0 && slab_id < static_cast<int>(slabs_.size())) {
                ids_to_reclaim.push_back(slab_id);
            }
        } else if (result == DequeueResult::Retry) {
            ++retry_count;
            continue;
        } else {
            empty_slab_queue_.reset_to_empty();
            break;
        }
    }

    // Under the current design no slab on the queue is the current slab
    // (only non-current slabs are enqueued; see SlabAllocator::deallocate
    // and ExpandableSlabAllocator::append_new_slab). All reclaimed slabs are
    // therefore destroyed, not reset.
    for (int slab_id : ids_to_reclaim) {
        if (slab_id == current_slab_id_) {
            throw PubSubItcException(
                "ExpandableSlabAllocator::drain_empty_slab_queue: "
                "current slab appeared in the empty-slab queue. "
                "This is a design invariant violation: the current slab must "
                "never be enqueued because it is self-reclaimed inline.");
        }
        slabs_[slab_id].reset();
    }
}

SlabAllocator* ExpandableSlabAllocator::append_new_slab() {
    // Switch the previously-current slab (if any) to non-current. From this
    // point on, deallocators that drive its count to zero will enqueue the
    // empty notification. If the slab's count is already zero now, the owner
    // thread must do the enqueue itself: no future deallocator will run on it.
    //
    // Either way, the try_claim_enqueue CAS ensures exactly one path performs
    // the enqueue (the deallocator that brought the count to zero, OR the
    // owner here).
    if (current_slab_id_ >= 0 && current_slab_id_ < static_cast<int>(slabs_.size())) {
        SlabAllocator* old_current = slabs_[current_slab_id_].get();
        if (old_current != nullptr) {
            old_current->clear_is_current();

            if (old_current->outstanding_count() == 0) {
                if (old_current->try_claim_enqueue()) {
                    old_current->queue_node().next.store(nullptr, std::memory_order_relaxed);
                    empty_slab_queue_.enqueue(&old_current->queue_node());
                }
            }
        }
    }

    int new_id = static_cast<int>(slabs_.size());
    slabs_.push_back(std::make_unique<SlabAllocator>(slab_size_, new_id, empty_slab_queue_));
    current_slab_id_ = new_id;
    return slabs_[current_slab_id_].get();
}

} // namespace pubsub_itc_fw
