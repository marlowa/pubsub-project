// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/SlabAllocator.hpp>

#include <cstdint>
#include <stdexcept>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

SlabAllocator::SlabAllocator(size_t slab_size, int slab_id, EmptySlabQueue& notify_queue)
    : slab_size_{slab_size}, slab_id_{slab_id}, notify_queue_{notify_queue} {
    if (slab_size_ == 0) {
        throw PreconditionAssertion("SlabAllocator: slab_size must be greater than zero", __FILE__, __LINE__);
    }

    memory_ = mmap(nullptr, slab_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory_ == MAP_FAILED) {
        throw std::bad_alloc();
    }

    queue_node_.slab_id = slab_id_;
    queue_node_.next.store(nullptr, std::memory_order_relaxed);
}

SlabAllocator::~SlabAllocator() {
    if (memory_ != MAP_FAILED) {
        munmap(memory_, slab_size_);
        memory_ = MAP_FAILED;
    }
}

void* SlabAllocator::allocate(size_t size) {
    if (size == 0) {
        throw PreconditionAssertion("SlabAllocator::allocate: size must be greater than zero", __FILE__, __LINE__);
    }

    constexpr size_t alignment = alignof(std::max_align_t);
    const size_t aligned_bump = (bump_ + alignment - 1) & ~(alignment - 1);
    const size_t new_bump = aligned_bump + size;

    if (new_bump > slab_size_) {
        return nullptr;
    }

    outstanding_allocations_count_.fetch_add(1, std::memory_order_relaxed);
    bump_ = new_bump;

    return static_cast<uint8_t*>(memory_) + aligned_bump;
}

void SlabAllocator::deallocate(void* ptr) {
    if (ptr == nullptr) {
        throw PreconditionAssertion("SlabAllocator::deallocate: ptr must not be nullptr", __FILE__, __LINE__);
    }

    const int old = outstanding_allocations_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (old != 1) {
        return; // Not the last outstanding chunk.
    }

    // Count has just transitioned to zero. The slab will be reclaimed via
    // the empty-slab queue only if it is no longer the current slab. The
    // current slab is held directly by the owner and is reset inline, not
    // through the queue.
    //
    // Reading is_current_ AFTER the fetch_sub establishes the ordering we
    // need: if the owner clears is_current_ before this load, we will see
    // false here and proceed to enqueue. If the owner clears is_current_
    // after this load, the owner's switch-away path will itself check the
    // count, see zero, and perform the enqueue. The try_claim_enqueue CAS
    // ensures exactly one of the two paths wins.
    if (is_current_.load(std::memory_order_acquire)) {
        return;
    }

    if (try_claim_enqueue()) {
        queue_node_.next.store(nullptr, std::memory_order_relaxed);
        notify_queue_.enqueue(&queue_node_);
    }
}

bool SlabAllocator::contains(const void* ptr) const {
    const auto* byte_ptr = static_cast<const uint8_t*>(ptr);
    const auto* start = static_cast<const uint8_t*>(memory_);
    const auto* end = start + slab_size_;
    return byte_ptr >= start && byte_ptr < end;
}

bool SlabAllocator::is_empty() const {
    return outstanding_allocations_count_.load(std::memory_order_acquire) == 0;
}

bool SlabAllocator::is_full() const {
    constexpr size_t alignment = alignof(std::max_align_t);
    const size_t aligned_bump = (bump_ + alignment - 1) & ~(alignment - 1);
    return aligned_bump >= slab_size_;
}

int SlabAllocator::slab_id() const {
    return slab_id_;
}

void SlabAllocator::reset() {
    bump_ = 0;
    outstanding_allocations_count_.store(0, std::memory_order_release);
    // queue_node_.next is owned by EmptySlabQueue and must not be touched here.
    // After dequeue, head_ points to this node as the new dummy; zeroing next
    // would sever any link the queue has already established through it.
    queue_node_.slab_id = slab_id_;
}

size_t SlabAllocator::capacity() const {
    return slab_size_;
}

size_t SlabAllocator::bytes_used() const {
    return bump_;
}

int SlabAllocator::outstanding_count() const {
    return outstanding_allocations_count_.load(std::memory_order_acquire);
}

bool SlabAllocator::is_current() const {
    return is_current_.load(std::memory_order_acquire);
}

void SlabAllocator::clear_is_current() {
    is_current_.store(false, std::memory_order_release);
}

bool SlabAllocator::try_claim_enqueue() {
    bool expected = false;
    return is_enqueued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
}

EmptySlabQueueNode& SlabAllocator::queue_node() {
    return queue_node_;
}

} // namespace pubsub_itc_fw
