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
#include <vector>

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
 * expandable memory allocation while maintaining lock-free performance for
 * the common case.
 *
 * POOL CHAIN STRUCTURE
 *    - Pools linked via next_pool_ pointers (singly-linked list).
 *    - head_pool_ points to the first pool in the chain.
 *    - current_pool_ptr_ points to the most recently active pool (optimisation).
 *    - cleanup_list_ holds unique_ptrs for RAII cleanup.
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
 *      acceptable — the slow path is the recovery mechanism for exactly
 *      this case.
 *
 *    - If the allocator is consistently seeing slow-path hits after drains,
 *      that is a signal that objects_per_pool or initial_pools is
 *      misconfigured for the workload, not a deficiency in the pointer
 *      strategy.
 *
 * EXPANSION STRATEGY
 *    - Starts with initial_pools pre-allocated.
 *    - Expands on demand when all pools are exhausted.
 *    - No hard limit – expands as long as the operating system can provide memory.
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
 *    - All FixedSizeMemoryPool instances are owned by this allocator.
 *    - Pools are added to the chain but never removed during the allocator's
 *      lifetime. This ensures that traversal of the pool chain is always safe.
 *    - All pools are destroyed together when the allocator itself is destroyed.
 *
 * THREAD SAFETY
 *    - allocate() is thread-safe (fast path lock-free, slow path mutex).
 *    - deallocate() is thread-safe (traversal uses atomic loads).
 *    - get_pool_statistics() is thread-safe (snapshot via atomic loads).
 *
 * MEMORY ORDERING GUARANTEES
 *    - current_pool_ptr_ is published using __ATOMIC_RELEASE. This ensures that
 *      all writes performed during pool creation, including linking the pool
 *      into the chain, become visible before other threads observe the new
 *      pointer.
 *
 *    - Threads reading current_pool_ptr_ use __ATOMIC_ACQUIRE, which guarantees
 *      that once a thread observes a new pool pointer, it also observes the
 *      fully initialised pool and its correct position in the chain.
 *
 *    - head_pool_ is updated under the expansion mutex and published using
 *      __ATOMIC_RELEASE. Readers use __ATOMIC_ACQUIRE to ensure that the pool
 *      chain is observed in a consistent state. Pools are only added, never
 *      removed, so readers may safely traverse the chain without additional
 *      synchronisation.
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
 *        backpressure mechanism — by the time it fires, the allocation has
 *        already succeeded.
 *
 *    - Because the allocator can grow without bound, the only true protection
 *      against unbounded memory growth is the high-watermark handler combined
 *      with appropriate producer-side flow control.
 *
 * CALLBACK SEMANTICS
 *    - Callback functions may be invoked from threads performing allocation or
 *      deallocation. They are not invoked concurrently by multiple threads at
 *      the same time.
 *
 *    - Callbacks are expected to be lightweight. They may log messages, and
 *      they may allocate memory if required, although this is not recommended
 *      for performance-critical paths.
 *
 *    - handler_for_pool_exhausted_ is invoked under expansion_mutex_ and
 *      therefore cannot be called concurrently by multiple threads.
 *
 *    - handler_for_invalid_free_ is invoked under callback_mutex_ and
 *      therefore cannot be called concurrently by multiple threads. The
 *      lock is taken only after the double-free or invalid-pointer condition
 *      has been confirmed atomically, so it does not appear on the normal
 *      deallocation path.
 *
 *    - handler_for_huge_pages_error_ is invoked from add_pool_to_chain(),
 *      which is called under expansion_mutex_, so it also cannot be called
 *      concurrently by multiple threads.
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
 * RACE CONDITION NOTES
 *    - Linking a new pool into the chain is safe. The next_pool_ pointer is
 *      published using __ATOMIC_RELEASE, and readers use __ATOMIC_ACQUIRE.
 *      This ensures that once a reader observes the new pool, it also observes
 *      the fully initialised chain.
 *
 *    - During expansion, the creating thread performs the first allocation from
 *      the new pool while still holding the expansion mutex. The pool is only
 *      published via current_pool_ptr_ after this allocation has succeeded.
 *      This prevents other threads from observing an empty pool and attempting
 *      to allocate from it simultaneously.
 *
 *    - The pool chain is traversed using __ATOMIC_ACQUIRE loads. Pools are only
 *      added, never removed, so traversal is safe even while another thread is
 *      expanding the chain.
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
     * @warning This precondition is not mechanically enforced. The fast path in
     *          allocate() does not hold expansion_mutex_, so taking the mutex
     *          in the destructor would not protect against concurrent allocations.
     *          Violating this precondition is undefined behaviour and will not
     *          be reliably detected at runtime.
     *
     * @note cleanup_list_ is traversed without a lock during destruction. This
     *       is safe only because of the precondition above. Any pool objects
     *       that still have allocated slots will have their objects destructed
     *       here — see FixedSizeMemoryPool destructor for details.
     */
    /**
     * @brief Destroys the allocator, checking canaries on any leaked objects.
     *
     * Sweeps all pools for slots still marked is_constructed (caller-leaked
     * objects). For each such slot, checks the canary before calling ~T().
     * If the canary is corrupt, handler_for_invalid_free_ is called and the
     * destructor is skipped for that slot — calling ~T() on a corrupt object
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
    ExpandablePoolAllocator(const std::string& pool_name, int objects_per_pool, int initial_pools, int expansion_threshold_hint,
                            std::function<void(void*, int)> handler_for_pool_exhausted,
                            std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error,
                            UseHugePagesFlag use_huge_pages_flag);

    ExpandablePoolAllocator(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator& operator=(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator(ExpandablePoolAllocator&&) = delete;
    ExpandablePoolAllocator& operator=(ExpandablePoolAllocator&&) = delete;

    /**
     * @brief Allocates and constructs an object.
     *
     * Fast path: attempts lock-free allocation from current_pool_ptr_.
     * Slow path: under mutex, searches all pools and expands if necessary.
     *
     * @return Pointer to newly constructed object, or nullptr if operating
     *         system memory is exhausted.
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
     * @param[in] obj Pointer to object to deallocate (nullptr is safe — throws
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
    FixedSizeMemoryPool<T>* add_pool_to_chain();

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
     * does NOT limit or prevent pool expansion — the allocator will
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

    FixedSizeMemoryPool<T>* head_pool_{nullptr};
    CacheLine<FixedSizeMemoryPool<T>*> current_pool_ptr_{nullptr};
    mutable std::mutex expansion_mutex_;
    std::vector<std::unique_ptr<FixedSizeMemoryPool<T>>> cleanup_list_;

    CacheLine<std::atomic<uint64_t>> total_allocations_;
    CacheLine<std::atomic<uint64_t>> fast_path_allocations_;
    CacheLine<std::atomic<uint64_t>> slow_path_allocations_;
    CacheLine<std::atomic<uint64_t>> expansion_events_;
    CacheLine<std::atomic<uint64_t>> failed_allocations_;
};

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(const std::string& pool_name,
                                                    int objects_per_pool, int initial_pools,
                                                    int expansion_threshold_hint,
                                                    std::function<void(void*, int)> handler_for_pool_exhausted,
                                                    std::function<void(void*, void*)> handler_for_invalid_free,
                                                    std::function<void(void*)> handler_for_huge_pages_error,
                                                    UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(pool_name)
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

    for (int i = 0; i < initial_pools_; ++i) {
        add_pool_to_chain();
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

    std::lock_guard<std::mutex> lock(expansion_mutex_);

    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);

    while (traverse != nullptr) {
        raw_mem = traverse->allocate();
        if (raw_mem != nullptr) {
            total_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            __atomic_store_n(&current_pool_ptr_.value, traverse, __ATOMIC_RELEASE);
            T* obj = ::new (static_cast<void*>(raw_mem)) T();
            std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
            is_constructed->store(1U, std::memory_order_release);
            return obj;
        }
        traverse = traverse->get_next_pool();
    }

    FixedSizeMemoryPool<T>* new_pool = add_pool_to_chain();
    if (new_pool != nullptr) {
        expansion_events_.value.fetch_add(1, std::memory_order_relaxed);
        raw_mem = new_pool->allocate();

        if (raw_mem != nullptr) {
            total_allocations_.value.fetch_add(1, std::memory_order_relaxed);
            __atomic_store_n(&current_pool_ptr_.value, new_pool, __ATOMIC_RELEASE);

            if (handler_for_pool_exhausted_ != nullptr) {
                handler_for_pool_exhausted_(nullptr, objects_per_pool_);
            }

            T* obj = ::new (static_cast<void*>(raw_mem)) T();
            std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
            is_constructed->store(1U, std::memory_order_release);
            return obj;
        }
    }

    failed_allocations_.value.fetch_add(1, std::memory_order_relaxed);

    // At this point, the operating system has refused to provide memory for a new pool.
    // On an overcommitting system this is already a catastrophic condition.
    throw PreconditionAssertion("ExpandablePoolAllocator::allocate: operating system refused memory for new pool", __FILE__, __LINE__);
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
    // In that case we must not call ~T() — the object may be corrupt and doing
    // so risks a secondary crash that would obscure the real failure. The slot
    // is deliberately not returned to the pool so the object pointer remains
    // valid for post-mortem examination in a core dump.
    const std::uint64_t* canary_ptr = get_canary_for_object(obj);
    if (*canary_ptr != slot_canary_value) {
        std::fprintf(stderr,
            "[ExpandablePoolAllocator] CANARY CORRUPTED in deallocate: "
            "pool '%s', object at %p. "
            "Expected canary 0x%016llX, found 0x%016llX. "
            "The T object wrote before its own start address. "
            "Slot is not returned to pool. "
            "Calling handler_for_invalid_free_.\n",
            pool_name_.c_str(),
            static_cast<void*>(obj),
            static_cast<unsigned long long>(slot_canary_value),
            static_cast<unsigned long long>(*canary_ptr));
        if (handler_for_invalid_free_ != nullptr) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    // --- Double-free detection ---
    std::atomic<std::uintptr_t>* is_constructed = get_is_constructed_for_object(obj);
    std::uintptr_t expected = 1U;
    if (!is_constructed->compare_exchange_strong(expected, 0U,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
        // is_constructed was already 0 — double free
        if (handler_for_invalid_free_ != nullptr) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    // Exactly one thread reaches here — safe to destruct and return to pool.
    obj->~T();
    owner_pool->deallocate(obj);
}

template <typename T> FixedSizeMemoryPool<T>* ExpandablePoolAllocator<T>::add_pool_to_chain() {
    auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(objects_per_pool_, use_huge_pages_flag_, [this](void* addr, std::size_t) {
        if (handler_for_huge_pages_error_ != nullptr) {
            handler_for_huge_pages_error_(addr);
        }
    });

    FixedSizeMemoryPool<T>* pool_ptr = new_pool.get();
    FixedSizeMemoryPool<T>* current_head = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);

    if (current_head == nullptr) {
        __atomic_store_n(&head_pool_, pool_ptr, __ATOMIC_RELEASE);
    } else {
        FixedSizeMemoryPool<T>* tail = current_head;
        while (tail->get_next_pool() != nullptr) {
            tail = tail->get_next_pool();
        }
        tail->set_next_pool(pool_ptr);
    }

    cleanup_list_.push_back(std::move(new_pool));

    return pool_ptr;
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

template <typename T>
ExpandablePoolAllocator<T>::~ExpandablePoolAllocator() {
    // Sweep all pools for caller-leaked objects (slots still marked is_constructed).
    // For each such slot, check the canary before calling ~T(). If the canary is
    // corrupt, call handler_for_invalid_free_ and clear is_constructed without
    // calling ~T() — doing so on a corrupt object risks a secondary crash.
    // The pool destructor (run when cleanup_list_ is destroyed) will then find
    // is_constructed already cleared and skip those slots cleanly.
    for (auto& pool_uptr : cleanup_list_) {
        pool_uptr->sweep_constructed_slots([this](T* obj, bool canary_ok) {
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
}

template <typename T>
std::atomic<std::uintptr_t>* ExpandablePoolAllocator<T>::get_is_constructed_for_object(T* obj) {
    using SlotType = Slot<T>;
    auto* byte_ptr = reinterpret_cast<char*>(obj);
    auto* slot = reinterpret_cast<SlotType*>(byte_ptr - offsetof(SlotType, storage));
    return &slot->is_constructed;
}

template <typename T>
const std::uint64_t* ExpandablePoolAllocator<T>::get_canary_for_object(T* obj) {
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
    for (auto* p = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
             p != nullptr;
             p = p->get_next_pool()) {
        pool_count++;
    }
    stats.per_pool_allocation_counts.counts.resize(pool_count);
    uint64_t i = 0;
    for (auto* p = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
             p != nullptr;
             p = p->get_next_pool()) {
        stats.per_pool_allocation_counts.counts[i++] = p->get_allocation_count();
    }

    return stats;
}

} // namespace pubsub_itc_fw
