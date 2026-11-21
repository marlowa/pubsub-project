#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/AllocatorBehaviourStatistics.hpp>
#include <pubsub_itc_fw/CacheLine.hpp>
#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

/*
 * ============================================================================
 * EXPANDABLE POOL ALLOCATOR DESIGN OVERVIEW
 * ============================================================================
 *
 * This allocator manages a chain of FixedSizeMemoryPool instances to provide
 * expandable memory allocation. No path through it takes a lock: not taking a
 * node from the current pool, not looking through the chain for one, and not
 * adding a pool to the chain. See WHY ADDING A POOL NEEDS NO LOCK below.
 *
 * Adding a pool during the trading day is nevertheless a fault: it means the
 * pools were sized too small for their queue, and the memory taken was not
 * planned for at the start of the day. The handler_for_pool_exhausted of every
 * component logs it at Error (docs/framework/pools_that_grow_without_a_lock.md,
 * section 4).
 *
 * POOL CHAIN STRUCTURE
 *    - Pools linked via next_pool_ pointers (singly-linked list).
 *    - head_pool_ points to the first pool in the chain. It is made by the
 *      constructor, which makes at least one pool, and never changes.
 *    - current_pool_ptr_ points to the most recently active pool (optimisation).
 *    - The chain owns its pools: the destructor deletes every pool in it.
 *
 * CURRENT POOL POINTER STRATEGY
 *    - current_pool_ptr_ always points to the most recently added pool.
 *      It is updated forward (toward newer pools) during expansion, but is
 *      never reset backward toward older pools after frees.
 *
 *    - This is a deliberate design decision. Under sustained high load,
 *      demand filled the earlier pools and forced expansion. Those earlier
 *      pools are likely to remain full or near-full while the system is
 *      under load. Pointing the fast path at the newest pool avoids
 *      repeatedly attempting pools that are unlikely to have free slots.
 *
 *    - In a burst-then-drain pattern (heavy allocation followed by freeing
 *      everything), the fast path will miss on the next burst and fall
 *      through to the slow path, which traverses the chain from head_pool_
 *      and updates current_pool_ptr_ when it finds a slot. This is
 *      acceptable -- the slow path is the recovery mechanism for exactly
 *      this case.
 *
 *    - If the allocator is consistently seeing slow-path hits after drains,
 *      that is a signal that objects_per_pool or initial_pools is
 *      misconfigured for the workload, not a deficiency in the pointer
 *      strategy.
 *
 * EXPANSION STRATEGY
 *    - Starts with initial_pools pre-allocated, at least one.
 *    - Expands on demand when all pools are exhausted.
 *    - No hard limit -- expands as long as the operating system can provide memory.
 *    - expansion_threshold_hint is for monitoring and alerts only.
 *
 * OBJECT LIFETIME MANAGEMENT
 *    - allocate() returns constructed objects (placement new applied).
 *    - deallocate() destructs objects before returning memory to the pool.
 *    - FixedSizeMemoryPool deals only with raw memory.
 *    - Callers must not use delete on objects returned by allocate().
 *
 * DEALLOCATION STRATEGY
 *    - Linear search through the chain to find the owning pool.
 *    - O(N) where N is the number of pools, but typically N is small.
 *    - Acceptable for environments where deallocation is off the critical path.
 *
 * POOL OWNERSHIP
 *    - All FixedSizeMemoryPool instances are owned by this allocator, through
 *      the chain: every pool made is linked into it (see below), and the
 *      destructor deletes every pool it finds there.
 *    - Pools are added to the chain but never removed during the allocator's
 *      lifetime. This ensures that traversal of the pool chain is always safe.
 *    - All pools are destroyed together when the allocator itself is destroyed.
 *
 * THREAD SAFETY
 *    - allocate() is thread-safe and takes no lock, on its fast path or its
 *      slow path.
 *    - deallocate() is thread-safe (traversal uses atomic loads).
 *    - get_pool_statistics() is thread-safe (snapshot via atomic loads).
 *
 * MEMORY ORDERING GUARANTEES
 *    - Every read and write of a pool's link to the next, and every write of
 *      current_pool_ptr_ on the slow path, is sequentially consistent
 *      (__ATOMIC_SEQ_CST). Every thread then sees those operations in one
 *      single order, which is what the TLA+ model of the chain assumes, so the
 *      model's result holds for this code. The slow path is rare, so the cost
 *      of the strongest ordering does not matter there.
 *
 *    - The fast path reads current_pool_ptr_ with __ATOMIC_ACQUIRE. It is only
 *      where to look first: whichever pool it names, the pool is fully made,
 *      because it was in the chain before any thread could name it.
 *
 * BACKPRESSURE AND MEMORY EXHAUSTION
 *    - The message queue is backed by an ExpandablePoolAllocator, which means
 *      the queue is never "full" in the traditional sense. When the current
 *      fixed-size memory pool is exhausted, the allocator transparently chains
 *      in a new pool and allocation continues. There is therefore no
 *      producer-side blocking or message dropping due to queue capacity.
 *
 *    - Two independent notification mechanisms exist, and must not be confused:
 *
 *      WATERMARK CALLBACKS (queue-depth layer):
 *        The high- and low-watermark callbacks on LockFreeMessageQueue fire
 *        based on the number of messages currently in the queue. They are the
 *        intended backpressure mechanism: the high-watermark callback signals
 *        that the consumer is falling behind and producers should slow down or
 *        shed load; the low-watermark callback signals recovery.
 *
 *      POOL EXHAUSTION HANDLER (memory layer):
 *        The handler_for_pool_exhausted callback fires when a FixedSizeMemoryPool
 *        slab within this allocator is exhausted and a new slab must be
 *        allocated from the heap. This is a memory-layer event entirely
 *        independent of queue depth. It signals that the allocator has grown
 *        beyond its pre-allocated capacity and that heap allocation is
 *        occurring. It may be used to log or raise an alert, but it is not a
 *        backpressure mechanism -- by the time it fires, the allocation has
 *        already succeeded.
 *
 *    - Because the allocator can grow without bound, the only true protection
 *      against unbounded memory growth is the high-watermark handler combined
 *      with appropriate producer-side flow control.
 *
 * CALLBACK SEMANTICS
 *    - Callback functions may be invoked from threads performing allocation or
 *      deallocation. handler_for_pool_exhausted_ and
 *      handler_for_huge_pages_error_ may be invoked by two threads at once,
 *      when both add a pool at once; every handler in the venue only logs,
 *      which the logger allows.
 *
 *    - Callbacks are expected to be lightweight. They may log messages, and
 *      they may allocate memory if required, although this is not recommended
 *      for performance-critical paths.
 *
 *    - handler_for_pool_exhausted_ is invoked by the thread that added a
 *      pool, after the pool is linked into the chain, holding no lock.
 *
 *    - handler_for_invalid_free_ is invoked under callback_mutex_ and
 *      therefore cannot be called concurrently by multiple threads. The
 *      lock is taken only after the double-free or invalid-pointer condition
 *      has been confirmed atomically, so it does not appear on the normal
 *      deallocation path.
 *
 *    - handler_for_huge_pages_error_ is invoked while a pool is being made,
 *      by the thread making it, holding no lock.
 *
 * DEALLOCATION COST
 *    - Deallocation performs a linear search through the pool chain to locate
 *      the owning pool. This operation is O(number_of_pools). This design is
 *      intentional and acceptable for systems where deallocation is not on the
 *      critical path.
 *
 * THREAD LIFETIME REQUIREMENT
 *    - The allocator must not be destroyed while other threads may still call
 *      allocate() or deallocate(). It is intended that each allocator instance
 *      is owned by a single application thread and destroyed only after that
 *      thread has completed all allocation and deallocation activity.
 *
 * WHY ADDING A POOL NEEDS NO LOCK
 *    An earlier version of this allocator took a mutex, expansion_mutex_, on
 *    its slow path: a thread that found the current pool full locked it, looked
 *    through the chain, and, if every pool was full, made a pool and linked it,
 *    all while holding the mutex. Every other thread that wanted a node from the
 *    same queue waited, and in a load test that waiting fell on the order path
 *    (BUG-0127). The mutex is not needed: each thing it guarded can be done
 *    safely without it, as the four points below explain, and a model checker
 *    has confirmed it (HOW THIS WAS CHECKED, below).
 *    docs/framework/pools_that_grow_without_a_lock.md sets the argument out in
 *    full.
 *
 *    1. Looking through the chain for a pool with a free node. Each pool's
 *       free list is lock-free already (FixedSizeMemoryPool, checked by
 *       docs/framework/tla/PoolFreeList.tla), so several threads may take
 *       nodes from one pool at once. Walking the chain only reads links, and a
 *       link changes once, from null to a pool, because pools are never
 *       removed; deallocate() has always walked the chain while pools are
 *       being added.
 *
 *    2. Making the pool found the current one. current_pool_ptr_ is only
 *       where to look first. Two threads that set it at once each set a pool
 *       that had a free node, and either is correct.
 *
 *    3. Adding a pool. A thread that finds every pool full makes a pool, and
 *       only then links it, by a compare-and-swap of the last pool's link from
 *       null to its own pool (FixedSizeMemoryPool::link_next_pool). When
 *       several threads find the chain full at the same moment and each tries
 *       to link a pool there, exactly one compare-and-swap succeeds. Each other
 *       thread learns from its failed compare-and-swap which pool is now
 *       linked there, walks on to the new end of the chain, and links its own
 *       pool there in the same way. So every pool made is linked, and linked
 *       once, because only the thread that made it links it and a link from
 *       null can be set only once. The chain cannot form a loop, because a
 *       link only ever points to a pool that was in no chain before. The fault
 *       known as ABA, in which a compare-and-swap is fooled by a value that
 *       changed and changed back, cannot happen, because no link ever returns
 *       to null and no pool is freed or reused while the allocator lives.
 *
 *    4. Taking a node only from a pool in the chain. A thread never takes a
 *       node from the pool it made before that pool is linked. A node handed
 *       out from a pool not yet linked could be freed by the consumer, whose
 *       deallocate() would walk the chain and not find its pool.
 *
 *    THREADS ARRIVING AT ONCE. Several threads finding the chain full at the
 *    same moment is not a race condition: no order of their steps gives a
 *    wrong result -- no pool is lost, no node is given to two threads, and
 *    every freed node finds its pool. The one thing that depends on the order
 *    of their steps is how many pools are added. Two threads that each find
 *    the chain full each add a pool, so more pools can be added than were
 *    strictly needed. Each extra pool is linked and is used by later
 *    allocations; the cost is memory taken sooner than it was needed, and it
 *    is bounded: the nodes in all the pools never exceed one pool's worth,
 *    plus the most nodes ever in use at once, plus one pool's worth for each
 *    thread allocating. Adding a pool during the trading day is in any case a
 *    fault of sizing, logged at Error, so this can only happen when a fault
 *    has already been reported.
 *
 *    HOW THIS WAS CHECKED. Tests cannot show that code shared by threads has
 *    no race condition: they pass while one remains, if the order of steps
 *    that shows it never happens to come up. ThreadSanitizer cannot check the
 *    pools' free lists either, because the builds for it and for Valgrind
 *    (USING_VALGRIND) replace each free list with a mutex, which those tools
 *    can follow; it does see the chain, whose compare-and-swap is an ordinary
 *    one. So the slow path was specified in TLA+,
 *    docs/framework/tla/PoolChain.tla, before this code was written, and
 *    checked with the model checker TLC. TLC does not sample orders of steps
 *    as a test does: within its bounds it tries every order in which the
 *    threads' steps can happen -- several producers allocating, finding the
 *    chain full and linking pools at the same moment, while a consumer frees
 *    nodes by walking the chain. Every property held: the chain never loops,
 *    no pool is linked twice, every node handed out belongs to a pool in the
 *    chain, the consumer always finds a node's pool, no node is handed out
 *    twice, no pool is lost, and the bound on the pools added holds. That is
 *    25,489,848 distinct states with three producers and four pools of one
 *    node, and 16,538,971 with three pools of two nodes. TLC also found each
 *    of two faults put in on purpose: a node handed out before its pool is
 *    linked, and a pool linked by a plain store instead of a compare-and-swap.
 *    The figures, and the steps TLC prints for each fault, are in
 *    docs/framework/tla/findings.md, section 5. The unit test
 *    PoolsAddedByManyThreadsAtOnceAreEachLinkedOnceAndEveryNodeIsFreed runs
 *    the same situation on this code, and fails when the compare-and-swap is
 *    replaced by a plain store.
 *
 *    The model assumes each step above is one atomic operation and that every
 *    thread sees them in one order; the sequentially consistent operations
 *    named under MEMORY ORDERING GUARANTEES give that. This code follows the
 *    model step for step: a change to the slow path is to be made to the
 *    model first and checked again with TLC.
 *
 * CANARY PROTECTION
 *    - Each Slot<T> (defined in FixedSizeMemoryPool.hpp) contains a canary
 *      field placed between is_constructed and the object storage. See the
 *      CANARY DESIGN section in FixedSizeMemoryPool.hpp for full details.
 *
 *    - deallocate() checks the canary before calling ~T(). If the canary is
 *      corrupt, handler_for_invalid_free_ is called and the object is neither
 *      destructed nor returned to the pool. The corrupted slot is deliberately
 *      left out of the pool so that the object pointer remains valid for
 *      post-mortem examination in a core dump.
 *
 * ============================================================================
 */

namespace pubsub_itc_fw {

template <typename T> class ExpandablePoolAllocator {
  public:
    /**
     * @brief Destroys the allocator and releases all pool memory.
     *
     * @pre No other thread may be calling allocate() or deallocate() when this
     *      destructor runs. The caller is responsible for ensuring that all
     *      threads using this allocator have completed their work and joined
     *      before this object is destroyed.
     *
     * @warning This precondition is not mechanically enforced. No path in
     *          allocate() takes a lock, so nothing the destructor could take
     *          would protect against concurrent allocations. Violating this
     *          precondition is undefined behaviour and will not be reliably
     *          detected at runtime.
     *
     * @note The chain is walked, and every pool in it deleted, without a lock
     *       during destruction. This is safe only because of the precondition
     *       above. Any pool objects that still have allocated slots will have
     *       their objects destructed here -- see FixedSizeMemoryPool destructor
     *       for details.
     */

    /**
     * @brief Destroys the allocator, checking canaries on any leaked objects.
     *
     * Sweeps all pools for slots still marked is_constructed (caller-leaked
     * objects). For each such slot, checks the canary before calling ~T().
     * If the canary is corrupt, handler_for_invalid_free_ is called and the
     * destructor is skipped for that slot -- calling ~T() on a corrupt object
     * risks a secondary crash that would obscure the real failure.
     *
     * @pre No other thread may be calling allocate() or deallocate() when
     *      this destructor runs.
     */
    ~ExpandablePoolAllocator();

    /** @ingroup allocator_subsystem */

    /**
     * @brief Constructs an expandable pool allocator.
     *
     * @param[in] pool_name Unique name for the pool (for statistics and logging).
     * @param[in] objects_per_pool Capacity of each individual pool.
     * @param[in] initial_pools Number of pools to pre-allocate at construction.
     * @param[in] expansion_threshold_hint Soft limit for monitoring (not enforced).
     * @param[in] handler_for_pool_exhausted Callback invoked when a new pool is added.
     * @param[in] handler_for_invalid_free Callback for invalid deallocate() calls,
     *            including double-free and canary corruption.
     * @param[in] handler_for_huge_pages_error Callback if huge page allocation fails.
     * @param[in] use_huge_pages_flag Whether to attempt 2MB huge page allocation.
     */
    ExpandablePoolAllocator(std::string pool_name, int objects_per_pool, int initial_pools, int expansion_threshold_hint,
                            std::function<void(void*, int)> handler_for_pool_exhausted, std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error, UseHugePagesFlag use_huge_pages_flag);

    ExpandablePoolAllocator(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator& operator=(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator(ExpandablePoolAllocator&&) = delete;
    ExpandablePoolAllocator& operator=(ExpandablePoolAllocator&&) = delete;

    /**
     * @brief Allocates and constructs an object.
     *
     * Fast path: takes a node from the pool current_pool_ptr_ names.
     * Slow path: walks the chain for a pool with a free node, and adds a pool
     * to the end of the chain if every pool is full. Neither takes a lock; see
     * WHY ADDING A POOL NEEDS NO LOCK at the top of this file.
     *
     * @return Pointer to the newly constructed object; never null. If the
     *         operating system refuses the memory for a new pool, it throws
     *         PreconditionAssertion.
     */
    [[nodiscard]] T* allocate();

    /**
     * @brief Destructs and deallocates an object.
     *
     * Searches the pool chain to find the owning pool and returns the memory.
     *
     * Performs the following checks before calling ~T():
     *   1. Canary check: if the slot canary has been corrupted (indicating a
     *      buffer underrun by the T object), handler_for_invalid_free_ is called
     *      and the function returns without calling ~T() or returning the slot
     *      to the pool. The corrupted slot is deliberately left out of the pool
     *      so the object pointer remains valid for core dump examination.
     *   2. Double-free detection: uses an atomic CAS on is_constructed to ensure
     *      exactly one thread calls ~T() per allocation. If is_constructed is
     *      already 0, handler_for_invalid_free_ is called.
     *
     * @param[in] obj Pointer to object to deallocate (nullptr is safe -- throws
     *            PreconditionAssertion).
     */
    void deallocate(T* obj);

    /**
     * @brief Collects a snapshot of the current allocation state.
     *
     * @return PoolStatistics containing pool count and allocation statistics.
     */
    [[nodiscard]] PoolStatistics get_pool_statistics() const;

    AllocatorBehaviourStatistics get_behaviour_statistics() const;

  private:
    /// Makes a pool, every node free, linked to nothing and known to no other thread.
    [[nodiscard]] FixedSizeMemoryPool<T>* make_pool();

    /**
     * @brief Links @p pool at the end of the chain, starting the walk at @p from, a pool in the chain.
     *
     * A compare-and-swap of the last pool's link from null to @p pool. If another thread linked a pool there first,
     * walks on to the new end and tries again, until @p pool is linked: the actions Link and ToEnd of
     * docs/framework/tla/PoolChain.tla.
     */
    static void link_at_end(FixedSizeMemoryPool<T>* from, FixedSizeMemoryPool<T>* pool);

    /**
     * @brief Returns a pointer to the is_constructed atomic flag for a given object.
     *
     * Performs reverse pointer arithmetic from the object pointer to locate the
     * Slot<T> that owns it, then returns a pointer to its is_constructed field.
     *
     * This relies on the Slot<T> layout being:
     *   [ is_constructed | canary | storage ]
     *
     * offsetof(SlotType, storage) accounts for both is_constructed and canary,
     * so this calculation remains correct regardless of the canary's presence.
     *
     * @param[in] obj Pointer to a live object previously returned by allocate().
     * @return Pointer to the is_constructed atomic for that object's slot.
     */
    static std::atomic<std::uintptr_t>* get_is_constructed_for_object(T* obj);

    /**
     * @brief Returns a pointer to the canary field for a given object.
     *
     * Performs reverse pointer arithmetic from the object pointer to locate the
     * Slot<T> that owns it, then returns a pointer to its canary field.
     *
     * @param[in] obj Pointer to a live object previously returned by allocate().
     * @return Pointer to the canary for that object's slot.
     */
    static const std::uint64_t* get_canary_for_object(T* obj);

    std::string pool_name_;
    int objects_per_pool_;
    int initial_pools_;

    /**
     * expansion_threshold_hint A soft monitoring hint only. This value
     * does NOT limit or prevent pool expansion -- the allocator will
     * always expand when demand exceeds capacity, as long as the
     * operating system provides memory. On Linux with overcommit
     * enabled, this means expansion is effectively unbounded.
     * This parameter is reserved for future use as a threshold at
     * which a monitoring callback could be triggered to alert the
     * operator that the pool has grown beyond its expected size.
     * If your workload is consistently exceeding this threshold,
     * increase objects_per_pool or initial_pools rather than
     * relying on expansion.
     */
    int expansion_threshold_hint_;

    UseHugePagesFlag use_huge_pages_flag_;
    mutable std::mutex callback_mutex_;
    std::function<void(void*, int)> handler_for_pool_exhausted_;
    std::function<void(void*, void*)> handler_for_invalid_free_;
    std::function<void(void*)> handler_for_huge_pages_error_;

    // The first pool, made by the constructor and never changed; the chain from it owns every pool.
    FixedSizeMemoryPool<T>* head_pool_{nullptr};
    CacheLine<FixedSizeMemoryPool<T>*> current_pool_ptr_{nullptr};

    CacheLine<std::atomic<uint64_t>> total_allocations_;
    CacheLine<std::atomic<uint64_t>> fast_path_allocations_;
    CacheLine<std::atomic<uint64_t>> slow_path_allocations_;
    CacheLine<std::atomic<uint64_t>> expansion_events_;
    CacheLine<std::atomic<uint64_t>> failed_allocations_;
};

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(std::string pool_name, int objects_per_pool, int initial_pools, int expansion_threshold_hint,
                                                    std::function<void(void*, int)> handler_for_pool_exhausted,
                                                    std::function<void(void*, void*)> handler_for_invalid_free,
                                                    std::function<void(void*)> handler_for_huge_pages_error, UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(std::move(pool_name))
    , objects_per_pool_(objects_per_pool)
    , initial_pools_(initial_pools)
    , expansion_threshold_hint_(expansion_threshold_hint)
    , use_huge_pages_flag_(use_huge_pages_flag)
    , handler_for_pool_exhausted_(std::move(handler_for_pool_exhausted))
    , handler_for_invalid_free_(std::move(handler_for_invalid_free))
    , handler_for_huge_pages_error_(std::move(handler_for_huge_pages_error)) {

    total_allocations_.value.store(0, std::memory_order_relaxed);
    fast_path_allocations_.value.store(0, std::memory_order_relaxed);
    slow_path_allocations_.value.store(0, std::memory_order_relaxed);
    expansion_events_.value.store(0, std::memory_order_relaxed);
    failed_allocations_.value.store(0, std::memory_order_relaxed);

    // The chain always has a first pool, which never changes, so that no thread ever has to make the first one: the
    // TLA+ model of the chain (docs/framework/tla/PoolChain.tla) starts with one pool.
    if (initial_pools_ < 1) {
        throw PreconditionAssertion("ExpandablePoolAllocator: initial_pools must be at least 1, got " + std::to_string(initial_pools_), __FILE__, __LINE__);
    }
    head_pool_ = make_pool();
    __atomic_store_n(&current_pool_ptr_.value, head_pool_, __ATOMIC_SEQ_CST);
    for (int i = 1; i < initial_pools_; ++i) {
        link_at_end(head_pool_, make_pool());
    }
}

template <typename T> T* ExpandablePoolAllocator<T>::allocate() {
    T* raw_mem = nullptr;
    FixedSizeMemoryPool<T>* pool = __atomic_load_n(&current_pool_ptr_.value, __ATOMIC_ACQUIRE);

    if (pool != nullptr) {
        raw_mem = pool->allocate();
        if (raw_mem != nullptr) {
            total_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            fast_path_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            T* obj = ::new (static_cast<void*>(raw_mem)) T();
            std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
            is_constructed->store(1U, std::memory_order_release);
            return obj;
        }
    }

    slow_path_allocations_.value.fetch_add(1, std::memory_order_relaxed);

    // The slow path takes no lock. It follows docs/framework/tla/PoolChain.tla step for step, and every read and
    // write of a link, and every write of current_pool_ptr_, is sequentially consistent, as the model assumes; see
    // WHY ADDING A POOL NEEDS NO LOCK at the top of this file.
    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_SEQ_CST);
    for (;;) {
        // Take a free node from the pool reached, and make that pool the current one (WalkStep in the model).
        raw_mem = traverse->allocate();
        if (raw_mem != nullptr) {
            total_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            __atomic_store_n(&current_pool_ptr_.value, traverse, __ATOMIC_SEQ_CST);
            T* obj = ::new (static_cast<void*>(raw_mem)) T();
            std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
            is_constructed->store(1U, std::memory_order_release);
            return obj;
        }
        // Or move on along the chain.
        FixedSizeMemoryPool<T>* next = traverse->get_next_pool();
        if (next != nullptr) {
            traverse = next;
            continue;
        }
        // Every pool is full: make a pool (Create), and link it at the end of the chain (Link, ToEnd). No node is
        // taken from it until it is linked, so that every node handed out belongs to a pool deallocate() can find.
        FixedSizeMemoryPool<T>* new_pool = nullptr;
        try {
            new_pool = make_pool();
        } catch (const std::exception&) {
            // At this point, the operating system has refused to provide memory for a new pool. On an overcommitting
            // system this is already a catastrophic condition.
            failed_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            throw PreconditionAssertion("ExpandablePoolAllocator::allocate: operating system refused memory for new pool", __FILE__, __LINE__);
        }
        link_at_end(traverse, new_pool);
        expansion_events_.value.fetch_add(1, std::memory_order_relaxed);
        if (handler_for_pool_exhausted_ != nullptr) {
            handler_for_pool_exhausted_(nullptr, objects_per_pool_);
        }
        // Walk on into the pool just linked. Another thread may have taken its nodes meanwhile, in which case the
        // walk goes on from there.
        traverse = new_pool;
    }
}

template <typename T> void ExpandablePoolAllocator<T>::deallocate(T* obj) {
    if (obj == nullptr) {
        throw PreconditionAssertion("deallocate called with nullptr", __FILE__, __LINE__);
    }

    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
    FixedSizeMemoryPool<T>* owner_pool = nullptr;

    while (traverse != nullptr) {
        if (traverse->contains(obj)) {
            owner_pool = traverse;
            break;
        }
        traverse = traverse->get_next_pool();
    }

    if (owner_pool == nullptr) {
        if (handler_for_invalid_free_ != nullptr) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    // --- Canary check ---
    // Check the canary before touching is_constructed or calling ~T().
    // A corrupt canary means the T object wrote before its own start address.
    // In that case we must not call ~T() -- the object may be corrupt and doing
    // so risks a secondary crash that would obscure the real failure. The slot
    // is deliberately not returned to the pool so the object pointer remains
    // valid for post-mortem examination in a core dump.
    const std::uint64_t* canary_ptr = get_canary_for_object(obj);
    if (*canary_ptr != slot_canary_value) {
        // fmt::print rather than fmt::format: print formats into a stack buffer and writes,
        // where format would return a std::string and so allocate. A corrupt canary means
        // something wrote out of bounds, and if that was the heap then allocating here could
        // fault inside the allocator and bury the real diagnostic under a secondary crash.
        fmt::print(stderr,
                   "[ExpandablePoolAllocator] CANARY CORRUPTED in deallocate: "
                   "pool '{}', object at {}. "
                   "Expected canary 0x{:016X}, found 0x{:016X}. "
                   "The T object wrote before its own start address. "
                   "Slot is not returned to pool. "
                   "Calling handler_for_invalid_free_.\n",
                   pool_name_, static_cast<void*>(obj), slot_canary_value, *canary_ptr);
        if (handler_for_invalid_free_ != nullptr) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    // --- Double-free detection ---
    std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
    std::uintptr_t expected = 1U;
    if (!is_constructed->compare_exchange_strong(expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire)) {
        // is_constructed was already 0 -- double free
        if (handler_for_invalid_free_ != nullptr) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    // Exactly one thread reaches here -- safe to destruct and return to pool.
    obj->~T();
    owner_pool->deallocate(obj);
}

template <typename T> FixedSizeMemoryPool<T>* ExpandablePoolAllocator<T>::make_pool() {
    // Owned by the chain once linked; until then by the thread making it, which always links it (link_at_end).
    return std::make_unique<FixedSizeMemoryPool<T>>(objects_per_pool_, use_huge_pages_flag_,
                                                    [this](void* addr, size_t) {
                                                        if (handler_for_huge_pages_error_ != nullptr) {
                                                            handler_for_huge_pages_error_(addr);
                                                        }
                                                    })
        .release();
}

template <typename T> void ExpandablePoolAllocator<T>::link_at_end(FixedSizeMemoryPool<T>* from, FixedSizeMemoryPool<T>* pool) {
    FixedSizeMemoryPool<T>* at = from;
    for (;;) {
        // Walk to the last pool (ToEnd).
        FixedSizeMemoryPool<T>* next = at->get_next_pool();
        while (next != nullptr) {
            at = next;
            next = at->get_next_pool();
        }
        // A compare-and-swap of its link from null to the pool (Link). On failure, the pool another thread linked
        // there is where the walk goes on from.
        FixedSizeMemoryPool<T>* already_linked = at->link_next_pool(pool);
        if (already_linked == nullptr) {
            return;
        }
        at = already_linked;
    }
}

template <typename T> PoolStatistics ExpandablePoolAllocator<T>::get_pool_statistics() const {
    PoolStatistics stats;
    stats.pool_name_ = pool_name_;
    stats.object_size_ = sizeof(T);
    stats.number_of_objects_per_pool_ = objects_per_pool_;
    stats.use_huge_pages_flag_ = use_huge_pages_flag_;

    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
    while (traverse != nullptr) {
        stats.number_of_pools_++;
        int available = traverse->get_number_of_available_objects();
        stats.number_of_allocated_objects_ += (objects_per_pool_ - available);
        stats.number_of_objects_available_ += available;
        if (traverse->is_full()) {
            stats.number_of_full_pools_++;
        }
        traverse = traverse->get_next_pool();
    }

    return stats;
}

template <typename T> ExpandablePoolAllocator<T>::~ExpandablePoolAllocator() {
    // Sweep all pools for caller-leaked objects (slots still marked is_constructed).
    // For each such slot, check the canary before calling ~T(). If the canary is
    // corrupt, call handler_for_invalid_free_ and clear is_constructed without
    // calling ~T() -- doing so on a corrupt object risks a secondary crash.
    // The pool destructor (run as each pool is deleted below) will then find
    // is_constructed already cleared and skip those slots cleanly.
    for (FixedSizeMemoryPool<T>* pool = head_pool_; pool != nullptr; pool = pool->get_next_pool()) {
        pool->sweep_constructed_slots([this](T* obj, bool canary_ok) {
            if (!canary_ok) {
                if (handler_for_invalid_free_ != nullptr) {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    handler_for_invalid_free_(nullptr, obj);
                }
            } else {
                obj->~T();
            }
        });
    }
    // The chain owns its pools: delete each, reading its link before it goes.
    FixedSizeMemoryPool<T>* pool = head_pool_;
    while (pool != nullptr) {
        FixedSizeMemoryPool<T>* next = pool->get_next_pool();
        const std::unique_ptr<FixedSizeMemoryPool<T>> owned(pool);
        pool = next;
    }
}

template <typename T> std::atomic<std::uintptr_t>* ExpandablePoolAllocator<T>::get_is_constructed_for_object(T* obj) {
    using SlotType = Slot<T>;
    auto* byte_ptr = reinterpret_cast<char*>(obj);
    auto* slot = reinterpret_cast<SlotType*>(byte_ptr - offsetof(SlotType, storage));
    return &slot->is_constructed;
}

template <typename T> const std::uint64_t* ExpandablePoolAllocator<T>::get_canary_for_object(T* obj) {
    using SlotType = Slot<T>;
    auto* byte_ptr = reinterpret_cast<char*>(obj);
    auto* slot = reinterpret_cast<SlotType*>(byte_ptr - offsetof(SlotType, storage));
    return &slot->canary;
}

template <typename T> AllocatorBehaviourStatistics ExpandablePoolAllocator<T>::get_behaviour_statistics() const {
    AllocatorBehaviourStatistics stats;

    stats.total_allocations = total_allocations_.value.load(std::memory_order_relaxed);
    stats.fast_path_allocations = fast_path_allocations_.value.load(std::memory_order_relaxed);
    stats.slow_path_allocations = slow_path_allocations_.value.load(std::memory_order_relaxed);
    stats.expansion_events = expansion_events_.value.load(std::memory_order_relaxed);
    stats.failed_allocations = failed_allocations_.value.load(std::memory_order_relaxed);

    uint64_t pool_count = 0;
    for (auto* p = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE); p != nullptr; p = p->get_next_pool()) {
        pool_count++;
    }
    stats.per_pool_allocation_counts.counts.resize(pool_count);
    uint64_t i = 0;
    for (auto* p = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE); p != nullptr; p = p->get_next_pool()) {
        stats.per_pool_allocation_counts.counts[i++] = p->get_allocation_count();
    }

    return stats;
}

} // namespaces
