#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * ExpandableSlabAllocator manages a chain of SlabAllocator instances, providing
 * variable-sized chunk allocation for the reactor's PDU transport layer.
 *
 * ==========================================================================
 * THREADING CONTRACT — READ THIS BEFORE USING THIS CLASS
 * ==========================================================================
 *
 * This allocator is designed for a specific two-thread concurrency model:
 *
 *   ALLOCATOR THREAD (reactor thread only):
 *     - allocate() must only ever be called from the reactor thread.
 *     - drain_empty_slab_queue() and append_new_slab() are called internally
 *       by allocate() and are therefore also reactor-thread-only.
 *     - Slab reset and slab destruction are performed exclusively by the
 *       reactor thread, at the start of allocate().
 *     - The bump pointer inside each SlabAllocator is written only by the
 *       reactor thread. No locking is required for bump pointer updates.
 *
 *   DEALLOCATOR THREADS (any application thread):
 *     - deallocate() may be called from any thread, including threads other
 *       than the reactor thread. Multiple threads may call deallocate()
 *       concurrently on different chunks from the same or different slabs.
 *     - deallocate() is thread-safe. It uses an atomic decrement on the
 *       slab's outstanding_allocations_count. No mutex is held.
 *     - When a thread's decrement transitions the count from 1 to 0, that
 *       thread enqueues the slab's ID into the empty_slab_queue_ (a lock-free
 *       MPSC queue). It does not reset or destroy the slab itself.
 *
 *   RECLAMATION (reactor thread only, demand-driven):
 *     - The reactor drains empty_slab_queue_ at the start of every allocate()
 *       call. This is the only point where slabs are reset or destroyed.
 *     - Reclamation is demand-driven: it happens when memory is needed, not
 *       on a background thread or timer tick. This guarantees progress under
 *       load and eliminates the GC-starvation failure mode seen in allocators
 *       that use a separate reclamation thread.
 *     - For each slab ID dequeued: it is destroyed (munmap) after the consumer
 *       advances head_ past it. To avoid a use-after-free against producers
 *       still in mid-enqueue, the MOST-RECENTLY popped slab is held over to
 *       the next drain (Vyukov sentinel pattern); only when a subsequent drain
 *       has confirmed head_ advanced past it is the held-over slab safe to
 *       destroy. This guarantees that no producer thread can hold a stale
 *       pointer to a destroyed slab's queue node as its `prev` value.
 *
 * CORRECTNESS INVARIANT:
 *   A slab can only become empty after the reactor has stopped allocating
 *   from it (i.e. after switching to a newer slab). Therefore there is no
 *   race between the reactor bump-allocating into a slab and an application
 *   thread decrementing that slab's count to zero.
 *
 * ==========================================================================
 * SLAB REGISTRY
 * ==========================================================================
 *
 * Slab IDs are monotonically increasing integers starting from 0. The slab
 * registry is a std::vector<unique_ptr<SlabAllocator>> indexed directly by
 * slab ID, giving O(1) lookup in deallocate(). Destroyed slabs leave a
 * nullptr entry in the vector. Attempting to deallocate() with a slab_id
 * whose entry is nullptr throws PreconditionAssertion.
 *
 * ==========================================================================
 * RETURN TYPE
 * ==========================================================================
 *
 * allocate() returns std::tuple<int, void*> for use with structured bindings:
 *
 *   auto [slab_id, ptr] = allocator.allocate(size);
 *
 * The slab_id must be passed back to deallocate() alongside the pointer.
 * This avoids pointer arithmetic and hidden metadata, and makes ownership
 * explicit at every call site.
 */

#include <atomic>
#include <cstddef>
#include <tuple>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Growable chain of mmap-backed slabs for variable-sized PDU chunk allocation.
 *
 * allocate() is reactor-thread-only and returns { slab_id, ptr } via structured
 * bindings. deallocate(slab_id, ptr) is thread-safe and may be called from any
 * application thread. Slab reclamation is demand-driven and occurs exclusively
 * on the reactor thread at the start of each allocate() call.
 *
 * See the THREADING CONTRACT block comment above for full concurrency semantics.
 */
class ExpandableSlabAllocator {
  public:
    /**
     * @brief Destroys all slabs owned by this allocator.
     *
     * Must only be called after all outstanding allocations have been freed
     * and no further allocate() or deallocate() calls will be made.
     */
    ~ExpandableSlabAllocator();

    /**
     * @brief Constructs an ExpandableSlabAllocator.
     *
     * Allocates the first slab immediately. All subsequent slabs are appended
     * on demand when the current slab is full.
     *
     * @param[in] slab_size Size of each slab in bytes. Must be greater than zero.
     *                      All slabs have the same size. Individual allocations
     *                      must not exceed this value.
     */
    explicit ExpandableSlabAllocator(size_t slab_size);

    ExpandableSlabAllocator(const ExpandableSlabAllocator&) = delete;
    ExpandableSlabAllocator& operator=(const ExpandableSlabAllocator&) = delete;

    /**
     * @brief Allocates a chunk of at least size bytes from the current slab.
     *
     * Must only be called from the reactor thread.
     *
     * Before allocating, drains the empty_slab_queue_ and reclaims any slabs
     * that have become empty since the last call. If the current slab is full,
     * appends a new slab and allocates from it.
     *
     * Returns the slab ID alongside the pointer so the caller can pass both
     * back to deallocate() without any pointer arithmetic or hidden metadata:
     *
     *   auto [slab_id, ptr] = allocator.allocate(payload_size);
     *
     * This function always returns a valid, non-null pointer. If the current
     * slab is full, a new slab is chained automatically. If allocation still
     * fails after chaining (e.g. mmap exhaustion), PubSubItcException is thrown.
     *
     * @param[in] size Number of bytes to allocate. Must be greater than zero
     *                 and must not exceed slab_size.
     * @return A tuple of { slab_id, ptr }. ptr is guaranteed non-null.
     * @pre size > 0 and size <= slab_size. Violating either throws PreconditionAssertion.
     */
    [[nodiscard]] std::tuple<int, void*> allocate(size_t size);

    /**
     * @brief Frees a chunk previously returned by allocate().
     *
     * Thread-safe. May be called from any thread concurrently with other
     * deallocate() calls. Must not be called from the reactor thread (the
     * reactor never frees chunks it has just allocated -- only application
     * threads do that).
     *
     * Atomically decrements the owning slab's outstanding_allocations_count.
     * If the count reaches zero, enqueues the slab ID into empty_slab_queue_
     * so the reactor can reclaim it at the next allocate() call. The calling
     * thread never resets or destroys the slab itself.
     *
     * @param[in] slab_id The slab ID returned by the corresponding allocate().
     * @param[in] ptr     The pointer returned by the corresponding allocate().
     *                    Must not be nullptr.
     * @pre slab_id must be in range, the slab must not have been destroyed,
     *      and ptr must not be nullptr. Violating any of these throws PreconditionAssertion.
     */
    void deallocate(int slab_id, void* ptr);

    /**
     * @brief Returns the number of slab registry slots (including destroyed slots).
     *
     * This value is monotonically increasing. Destroyed slabs leave nullptr
     * entries in the registry, so slab_count() may exceed the number of live
     * slabs.
     */
    [[nodiscard]] int slab_count() const;

    /**
     * @brief Returns the configured slab size in bytes.
     */
    [[nodiscard]] size_t slab_size() const;

  private:
    // =======================================================================
    // Segmented slab registry
    //
    // Slab IDs are assigned monotonically starting from 0. The registry is a
    // two-level structure: a fixed-size directory of atomic page pointers, each
    // pointing to a heap-allocated page of atomic SlabAllocator pointers.
    //
    // WHY NOT std::vector<unique_ptr<SlabAllocator>>?
    // std::vector::push_back() can trigger internal reallocation (freeing the
    // old backing array) while worker threads concurrently read element pointers
    // from it via deallocate(). That is an unsynchronised access to freed memory.
    //
    // With the segmented design:
    //   - The directory (pages_[]) is a fixed-size in-object array: it never
    //     moves or is freed for the lifetime of the allocator.
    //   - Each Page is heap-allocated once and never freed until the destructor
    //     runs. Workers load the page pointer with acquire, then load the slab
    //     pointer with acquire — both are stable once written.
    //   - The reactor writes page pointers and slab pointers with release so
    //     workers see a consistent view.
    // =======================================================================

    // 256 slots per page (page_bits=8 → page_size=256).
    // 1024 pages max → 262,144 distinct slab IDs before capacity exhaustion.
    static constexpr int page_bits = 8;
    static constexpr int page_size = 1 << page_bits;
    static constexpr int max_pages = 1024;

    struct Page {
        std::atomic<SlabAllocator*> slots[page_size];
        Page() {
            for (auto& s : slots) {
                s.store(nullptr, std::memory_order_relaxed);
            }
        }
    };

    void drain_empty_slab_queue();
    SlabAllocator* append_new_slab();
    [[nodiscard]] SlabAllocator* load_slab_reactor(int slab_id) const; // reactor thread only

    size_t slab_size_;
    int current_slab_id_{-1};
    EmptySlabQueue empty_slab_queue_;
    int slab_slot_count_{0};              // total slab IDs ever issued (monotonically increasing)
    std::atomic<Page*> pages_[max_pages]; // directory; initialised to nullptr in constructor

    // Vyukov sentinel reclamation: the most-recently-popped slab is kept alive
    // until the next drain confirms head_ has moved past it.
    // -1 means no slab is currently deferred.
    int deferred_reclaim_slab_id_{-1};
};

} // namespaces
