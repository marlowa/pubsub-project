#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * ExpandableSlabAllocator manages a chain of SlabAllocator instances, providing
 * variable-sized chunk allocation for the reactor's PDU transport layer.
 *
 * ==========================================================================
 * THREADING CONTRACT -- READ THIS BEFORE USING THIS CLASS
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
 * Slabs live in registry SLOTS, indexed directly for O(1) lookup in deallocate(). The
 * registry is a two-level directory of fixed size -- it must be readable without a lock by
 * any deallocating thread, and must never reallocate under those readers, which rules out a
 * growable container.
 *
 * SLOTS ARE RECYCLED. When a slab is destroyed its slot is returned to an intrusive free
 * list threaded through the slots themselves, and the next slab to be chained takes it back.
 * The free list needs no synchronisation because both ends are reactor-thread-only: slots are
 * released in drain_empty_slab_queue() and claimed in append_new_slab().
 *
 * That is what keeps a fixed directory from becoming a limit on process lifetime. Issuing
 * slots monotonically instead would make the directory a ceiling on the number of slab
 * rotations, and therefore on the total bytes a process could ever receive -- roughly 16 GiB
 * at a 64 KB slab, which sustained load reaches inside hours.
 *
 * ==========================================================================
 * RETURN TYPE
 * ==========================================================================
 *
 * allocate() returns std::tuple<SlabHandle, void*> for use with structured bindings:
 *
 *   auto [handle, ptr] = allocator.allocate(size);
 *
 * The handle must be passed back to deallocate() alongside the pointer. This avoids pointer
 * arithmetic and hidden metadata, and makes ownership explicit at every call site.
 *
 * A handle is a slot plus the generation that slot was in when the handle was issued, not a
 * bare slot number, because slots are reused. Releasing a slot bumps its generation, so a
 * handle that outlives its slab no longer matches and deallocate() rejects it instead of
 * freeing into whichever slab now occupies the slot. See SlabHandle.hpp.
 */

#include <atomic>
#include <cstddef>
#include <tuple>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>
#include <pubsub_itc_fw/SlabHandle.hpp>

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
/**
 * @brief Whether the empty-slab drain loop has genuinely failed to make progress.
 *
 * Both conditions are required. Elapsed time on its own does not describe progress: a
 * thread the scheduler has not run has made none, and that is a different condition from a
 * queue that will not drain. The two are separable by iteration count -- a self-loop or a
 * stuck producer reaches millions of iterations well inside the budget, a descheduled
 * thread reaches one.
 *
 * A free function, and public, because this decision is the part worth testing on its own:
 * the interesting case requires more than a second to elapse between two adjacent
 * statements, which cannot be arranged by calling drain_empty_slab_queue().
 *
 * @param[in] loop_iterations   Times round the drain loop so far.
 * @param[in] deadline_exceeded Whether the wall-clock budget has been spent.
 * @return True only when the budget is spent AND the loop has actually spun.
 */
[[nodiscard]] bool drain_loop_has_stalled(int64_t loop_iterations, bool deadline_exceeded);

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
     * Returns a slab handle alongside the pointer so the caller can pass both
     * back to deallocate() without any pointer arithmetic or hidden metadata:
     *
     *   auto [handle, ptr] = allocator.allocate(payload_size);
     *
     * This function always returns a valid, non-null pointer. If the current
     * slab is full, a new slab is chained automatically. If allocation still
     * fails after chaining (e.g. mmap exhaustion), PubSubItcException is thrown.
     *
     * @param[in] size Number of bytes to allocate. Must be greater than zero
     *                 and must not exceed slab_size.
     * @return A tuple of { handle, ptr }. ptr is guaranteed non-null.
     * @pre size > 0 and size <= slab_size. Violating either throws PreconditionAssertion.
     */
    [[nodiscard]] std::tuple<SlabHandle, void*> allocate(size_t size);

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
     * @param[in] handle The handle returned by the corresponding allocate().
     * @param[in] ptr    The pointer returned by the corresponding allocate().
     *                   Must not be nullptr.
     * @pre The handle's slot must be in range, the slab must not have been destroyed, the
     *      handle's generation must match the slot's current one, and ptr must not be
     *      nullptr. Violating any of these throws PreconditionAssertion.
     */
    void deallocate(SlabHandle handle, void* ptr);

    /**
     * @brief Returns the number of registry slots ever brought into use.
     *
     * This is the high-water mark of slots, not a count of slabs created: recycled slots are
     * counted once. It therefore stops growing once the process reaches its steady-state
     * number of concurrently live slabs, which is what makes the registry a bound on
     * concurrency rather than on lifetime.
     */
    [[nodiscard]] int slab_count() const;

    /**
     * @brief Returns how many registry slots are on the free list awaiting reuse.
     *
     * Diagnostic, reactor-thread-only. Present so tests can assert that slots are genuinely
     * recycled rather than merely not exhausted.
     */
    [[nodiscard]] int free_slot_count() const;

    /**
     * @brief Returns the configured slab size in bytes.
     */
    [[nodiscard]] size_t slab_size() const;

  private:
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
    //     pointer with acquire -- both are stable once written.
    //   - The reactor writes page pointers and slab pointers with release so
    //     workers see a consistent view.

    // 256 slots per page (page_bits=8 -> page_size=256).
    // 1024 pages max -> 262,144 distinct slab IDs before capacity exhaustion.
    static constexpr int page_bits = 8;
    static constexpr int page_size = 1 << page_bits;
    static constexpr int max_pages = 1024;

    struct Page {
        std::atomic<SlabAllocator*> slots[page_size];

        // Incremented every time a slot is handed to a new slab, so a handle issued against
        // the slot's previous occupant no longer matches. Atomic because deallocate() reads
        // it from any thread; the reactor is the only writer.
        std::atomic<uint32_t> generations[page_size];

        // Intrusive free list of released slots: next_free[slot] is the next free slot, or
        // -1 at the end. Deliberately a plain int array rather than a container -- a vector
        // would reallocate as it grew, which is the one thing this registry may never do
        // while readers are live. Needs no synchronisation: slots are released in
        // drain_empty_slab_queue() and claimed in append_new_slab(), both reactor-only.
        int next_free[page_size];

        Page() {
            for (auto& s : slots) {
                s.store(nullptr, std::memory_order_relaxed);
            }
            for (auto& g : generations) {
                g.store(0, std::memory_order_relaxed);
            }
            for (auto& n : next_free) {
                n = -1;
            }
        }
    };

    void drain_empty_slab_queue();
    SlabAllocator* append_new_slab();
    [[nodiscard]] SlabAllocator* load_slab_reactor(int slot) const; // reactor thread only
    [[nodiscard]] Page* page_for_slot(int slot) const;              // reactor thread only
    void release_slot(int slot);                                    // reactor thread only

    size_t slab_size_;
    int current_slab_slot_{-1};
    uint32_t current_slab_generation_{0};
    EmptySlabQueue empty_slab_queue_;
    int slab_slot_count_{0};              // high-water mark of slots in use; recycled slots counted once
    int free_slot_head_{-1};              // head of the intrusive free list, -1 when empty
    int free_slot_count_{0};              // diagnostic only
    std::atomic<Page*> pages_[max_pages]; // directory; initialised to nullptr in constructor

    // Vyukov sentinel reclamation: the most-recently-popped slab is kept alive
    // until the next drain confirms head_ has moved past it.
    // -1 means no slab is currently deferred.
    int deferred_reclaim_slot_{-1};
};

} // namespaces
