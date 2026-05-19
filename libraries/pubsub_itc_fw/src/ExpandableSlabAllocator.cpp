// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

ExpandableSlabAllocator::ExpandableSlabAllocator(size_t slab_size) : slab_size_{slab_size} {
    if (slab_size_ == 0) {
        throw PreconditionAssertion("ExpandableSlabAllocator: slab_size must be greater than zero", __FILE__, __LINE__);
    }

    for (auto& p : pages_) {
        p.store(nullptr, std::memory_order_relaxed);
    }

    append_new_slab();
}

ExpandableSlabAllocator::~ExpandableSlabAllocator() {
    for (int p = 0; p < kMaxPages; ++p) {
        Page* page = pages_[p].load(std::memory_order_relaxed);
        if (page == nullptr) {
            break; // pages are allocated sequentially; first null means no more pages
        }
        for (auto& slot : page->slots) {
            delete slot.load(std::memory_order_relaxed);
        }
        delete page;
    }
}

SlabAllocator* ExpandableSlabAllocator::load_slab_reactor(int slab_id) const noexcept {
    const int page_idx = slab_id >> kPageBits;
    const int slot_idx = slab_id & (kPageSize - 1);
    Page* page = pages_[page_idx].load(std::memory_order_relaxed);
    return page->slots[slot_idx].load(std::memory_order_relaxed);
}

std::tuple<int, void*> ExpandableSlabAllocator::allocate(size_t size) {
    if (size == 0) {
        throw PreconditionAssertion("ExpandableSlabAllocator::allocate: size must be greater than zero", __FILE__, __LINE__);
    }

    if (size > slab_size_) {
        throw PreconditionAssertion("ExpandableSlabAllocator::allocate: size exceeds slab_size", __FILE__, __LINE__);
    }

    drain_empty_slab_queue();

    SlabAllocator* current = load_slab_reactor(current_slab_id_);

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
        throw PubSubItcException("ExpandableSlabAllocator::allocate: allocation failed after chaining a new slab — "
                                 "this should not happen; mmap may have failed during slab construction");
    }

    return {current_slab_id_, ptr};
}

void ExpandableSlabAllocator::deallocate(int slab_id, void* ptr) {
    if (ptr == nullptr) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: ptr must not be nullptr", __FILE__, __LINE__);
    }

    if (slab_id < 0 || slab_id >= kMaxPages * kPageSize) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: slab_id out of range", __FILE__, __LINE__);
    }

    const int page_idx = slab_id >> kPageBits;
    const int slot_idx = slab_id & (kPageSize - 1);

    // Acquire page pointer published by the reactor with release.
    Page* page = pages_[page_idx].load(std::memory_order_acquire);
    if (page == nullptr) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: slab_id refers to an unallocated page", __FILE__, __LINE__);
    }

    // Acquire slab pointer. The reactor stores with release on slab creation and
    // stores nullptr with release on slab destruction.
    SlabAllocator* slab = page->slots[slot_idx].load(std::memory_order_acquire);
    if (slab == nullptr) {
        throw PreconditionAssertion("ExpandableSlabAllocator::deallocate: slab has already been destroyed", __FILE__, __LINE__);
    }

    slab->deallocate(ptr);
}

int ExpandableSlabAllocator::slab_count() const {
    return slab_slot_count_;
}

size_t ExpandableSlabAllocator::slab_size() const {
    return slab_size_;
}

void ExpandableSlabAllocator::drain_empty_slab_queue() {
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
    // corruption. Rather than spin until OOM, fail fast with a descriptive
    // exception so the reactor terminates cleanly and the operator can see
    // what went wrong.
    //
    // Note on the threshold: it is in wall-clock time, not iteration count. A
    // tight retry loop on a modern CPU runs in tens of nanoseconds per
    // iteration, so a sub-millisecond preemption window can easily cover
    // hundreds of thousands of iterations. One second is a generous budget
    // that allows any legitimate preemption to be resolved by the kernel
    // scheduler.
    int64_t loop_iterations = 0;
    int64_t retry_count = 0;
    int64_t got_item_count = 0;
    int last_got_slab_id = -1;
    int64_t same_id_repeats = 0;
    const auto tripwire_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    for (;;) {
        ++loop_iterations;
        if (std::chrono::steady_clock::now() > tripwire_deadline) {
            throw PubSubItcException("ExpandableSlabAllocator::drain_empty_slab_queue: "
                                     "tripwire exceeded; queue is not making progress for over one second. "
                                     "Likely a corrupted lock-free queue state (self-loop or stuck producer). "
                                     "Diagnostics: got_item=" +
                                     std::to_string(got_item_count) + " retry=" + std::to_string(retry_count) +
                                     " iterations=" + std::to_string(loop_iterations) + " last_slab_id=" + std::to_string(last_got_slab_id) +
                                     " same_id_repeats=" + std::to_string(same_id_repeats) + " live_slabs=" + std::to_string(slab_slot_count_) +
                                     " ids_to_reclaim.size=" + std::to_string(ids_to_reclaim.size()));
        }

        int slab_id = -1;
        DequeueResult result = empty_slab_queue_.try_dequeue(slab_id);

        if (result == DequeueResult::GotItem) {
            ++got_item_count;
            if (slab_id == last_got_slab_id) {
                ++same_id_repeats;
            } else {
                last_got_slab_id = slab_id;
                same_id_repeats = 0;
            }
            if (slab_id >= 0 && slab_id < slab_slot_count_) {
                ids_to_reclaim.push_back(slab_id);
            }
        } else if (result == DequeueResult::Retry) {
            ++retry_count;
            continue;
        } else {
            // Empty. Exit the loop. We deliberately do NOT call
            // EmptySlabQueue::reset_to_empty(): doing so would race with any
            // producer that did tail_.exchange(node) since our last drain.
            // Such a producer holds prev = (what tail_ was before its
            // exchange) and is about to do prev->next.store(node). If we
            // overwrite tail_ to point at the queue's dummy_ in between, we
            // both (a) lose the producer's exchange (their node is now
            // unreachable from tail_) and (b) silently allow the producer's
            // later store to land on a node that we may be about to destroy
            // in the reclamation phase below. The Vyukov pattern handles
            // this correctly without any reset: the popped node naturally
            // becomes the new sentinel (head_/tail_ point at it) and stays
            // alive until the NEXT drain confirms head_ has moved past it.
            break;
        }
    }

    // Vyukov sentinel reclamation. The most-recently-popped slab in this
    // drain becomes the queue's new sentinel: head_ ends pointing at its
    // node, and any producer in mid-enqueue may hold its node as `prev`. We
    // hold that slab over to the NEXT drain. The slab that was deferred from
    // the PREVIOUS drain is now safe to destroy: a successful GotItem in
    // this drain means head_ has advanced past it, which in turn means the
    // producer that put a successor in front of it has completed its store
    // (and therefore no producer holds the previously-deferred slab's node
    // as `prev` any longer).
    //
    // If this drain popped nothing (got_item_count == 0), head_ has not
    // moved; the previously-deferred slab remains deferred.
    if (!ids_to_reclaim.empty()) {
        const int new_deferred = ids_to_reclaim.back();
        ids_to_reclaim.pop_back();
        if (deferred_reclaim_slab_id_ >= 0) {
            ids_to_reclaim.push_back(deferred_reclaim_slab_id_);
        }
        deferred_reclaim_slab_id_ = new_deferred;
    }

    // Under the current design no slab on the queue is the current slab
    // (only non-current slabs are enqueued; see SlabAllocator::deallocate
    // and ExpandableSlabAllocator::append_new_slab). All reclaimed slabs are
    // therefore destroyed, not reset.
    for (int id : ids_to_reclaim) {
        if (id == current_slab_id_) {
            throw PubSubItcException("ExpandableSlabAllocator::drain_empty_slab_queue: "
                                     "current slab appeared in the empty-slab queue. "
                                     "This is a design invariant violation: the current slab must "
                                     "never be enqueued because it is self-reclaimed inline.");
        }
        const int page_idx = id >> kPageBits;
        const int slot_idx = id & (kPageSize - 1);
        Page* page = pages_[page_idx].load(std::memory_order_relaxed);
        SlabAllocator* slab = page->slots[slot_idx].load(std::memory_order_relaxed);
        // Release-store so workers that load with acquire see nullptr for this slot.
        page->slots[slot_idx].store(nullptr, std::memory_order_release);
        delete slab;
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
    if (current_slab_id_ >= 0) {
        SlabAllocator* old_current = load_slab_reactor(current_slab_id_);
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

    if (slab_slot_count_ >= kMaxPages * kPageSize) {
        throw PubSubItcException("ExpandableSlabAllocator::append_new_slab: slab slot capacity exhausted "
                                 "(max " + std::to_string(kMaxPages * kPageSize) + " slab IDs). "
                                 "This indicates an extraordinary number of slab rotations; "
                                 "check for runaway allocation or insufficient slab reclamation.");
    }

    const int new_id = slab_slot_count_++;
    const int page_idx = new_id >> kPageBits;
    const int slot_idx = new_id & (kPageSize - 1);

    // Allocate the page if this is the first slot in it.
    if (pages_[page_idx].load(std::memory_order_relaxed) == nullptr) {
        Page* new_page = new Page();
        // Release-store so workers can acquire the page pointer before accessing slots.
        pages_[page_idx].store(new_page, std::memory_order_release);
    }

    SlabAllocator* new_slab = new SlabAllocator(slab_size_, new_id, empty_slab_queue_);
    // Release-store so workers that load with acquire see the fully constructed slab.
    pages_[page_idx].load(std::memory_order_relaxed)->slots[slot_idx].store(new_slab, std::memory_order_release);

    current_slab_id_ = new_id;
    return new_slab;
}

} // namespace pubsub_itc_fw
