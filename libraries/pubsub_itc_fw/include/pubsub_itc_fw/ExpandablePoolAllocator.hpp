#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>
#include <atomic>

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
 * CALLBACK SEMANTICS
 *    - Callback functions may be invoked from threads performing allocation or
 *      deallocation. They are not invoked concurrently by multiple threads at
 *      the same time.
 *
 *    - Callbacks are expected to be lightweight. They may log messages, and
 *      they may allocate memory if required, although this is not recommended
 *      for performance-critical paths.
 *
 *    - Callbacks are not required to be noexcept.
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
 * ============================================================================
 */

namespace pubsub_itc_fw {

template <typename T> class ExpandablePoolAllocator final {
  public:
    ~ExpandablePoolAllocator() = default;

    /**
     * @brief Constructs an expandable pool allocator.
     *
     * @param[in] pool_name Unique name for the pool (for statistics and logging).
     * @param[in] objects_per_pool Capacity of each individual pool.
     * @param[in] initial_pools Number of pools to pre-allocate at construction.
     * @param[in] expansion_threshold_hint Soft limit for monitoring (not enforced).
     * @param[in] handler_for_pool_exhausted Callback invoked when a new pool is added.
     * @param[in] handler_for_invalid_free Callback for invalid deallocate() calls.
     * @param[in] handler_for_huge_pages_error Callback if huge page allocation fails.
     * @param[in] use_huge_pages_flag Whether to attempt 2MB huge page allocation.
     */
    ExpandablePoolAllocator(std::string const& pool_name, int objects_per_pool, int initial_pools,
                            int expansion_threshold_hint,
                            std::function<void(void*, int)> handler_for_pool_exhausted,
                            std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error,
                            UseHugePagesFlag use_huge_pages_flag);

    ExpandablePoolAllocator(ExpandablePoolAllocator const&) = delete;
    ExpandablePoolAllocator& operator=(ExpandablePoolAllocator const&) = delete;
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
     * Performs double-free detection using the per-slot flag. Invokes
     * handler_for_invalid_free if the pointer does not belong to any pool
     * or if a double free is detected.
     *
     * @param[in] obj Pointer to object to deallocate (nullptr is safe).
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

    static std::uintptr_t* get_flag_for_object(T* obj);

    std::string pool_name_;
    int objects_per_pool_;
    int initial_pools_;
    int expansion_threshold_hint_;
    UseHugePagesFlag use_huge_pages_flag_;
    std::function<void(void*, int)> handler_for_pool_exhausted_;
    std::function<void(void*, void*)> handler_for_invalid_free_;
    std::function<void(void*)> handler_for_huge_pages_error_;

    FixedSizeMemoryPool<T>* head_pool_{nullptr};
    CacheLine<FixedSizeMemoryPool<T>*> current_pool_ptr_{nullptr};
    mutable std::mutex expansion_mutex_;
    std::vector<std::unique_ptr<FixedSizeMemoryPool<T>>> cleanup_list_;

    CacheLine<std::atomic<uint64_t>> total_allocations_{0};
    CacheLine<std::atomic<uint64_t>> fast_path_allocations_{0};
    CacheLine<std::atomic<uint64_t>> slow_path_allocations_{0};
    CacheLine<std::atomic<uint64_t>> expansion_events_{0};
    CacheLine<std::atomic<uint64_t>> failed_allocations_{0};
};

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(std::string const& pool_name, //
                                                    int objects_per_pool, int initial_pools,
                                                    int expansion_threshold_hint, //
                                                    std::function<void(void*, int)> handler_for_pool_exhausted, //
                                                    std::function<void(void*, void*)> handler_for_invalid_free, //
                                                    std::function<void(void*)> handler_for_huge_pages_error, //
                                                    UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(pool_name)
    , objects_per_pool_(objects_per_pool)
    , initial_pools_(initial_pools)
    , expansion_threshold_hint_(expansion_threshold_hint)
    , use_huge_pages_flag_(use_huge_pages_flag)
    , handler_for_pool_exhausted_(std::move(handler_for_pool_exhausted))
    , handler_for_invalid_free_(std::move(handler_for_invalid_free))
    , handler_for_huge_pages_error_(std::move(handler_for_huge_pages_error)) {
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
            std::uintptr_t* flag = get_flag_for_object(obj);
            *flag = 1U;
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
            std::uintptr_t* flag = get_flag_for_object(obj);
            *flag = 1U;
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
            std::uintptr_t* flag = get_flag_for_object(obj);
            *flag = 1U;
            return obj;
        }
    }

    failed_allocations_.value.fetch_add(1, std::memory_order_relaxed);

    // At this point, the operating system has refused to provide memory for a new pool.
    // On an overcommitting system this is already a catastrophic condition: the process cannot reasonably continue.
    // Treat this as a hard precondition failure rather than a recoverable runtime condition.
    // It is a precondition that the OS will always give us the memory we need.
    // We have to run on a machine where we can be very confident indeed that this is the case.
    // Strictly speaking, this is a post condition violation.
    throw PreconditionAssertion("ExpandablePoolAllocator::allocate: operating system refused memory for new pool",
                                __FILE__, __LINE__);
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
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    std::uintptr_t* flag = get_flag_for_object(obj);

    if (*flag == 0U) {
        if (handler_for_invalid_free_ != nullptr) {
            handler_for_invalid_free_(nullptr, obj);
        }
        return;
    }

    obj->~T();
    *flag = 0U;
    owner_pool->deallocate(obj);
}

template <typename T> FixedSizeMemoryPool<T>* ExpandablePoolAllocator<T>::add_pool_to_chain() {
    auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(objects_per_pool_, use_huge_pages_flag_, [this](void* addr, std::size_t) {
        if (handler_for_huge_pages_error_ != nullptr) {
            handler_for_huge_pages_error_(addr);
        }
    });

    FixedSizeMemoryPool<T>* pool_ptr = new_pool.get();
    FixedSizeMemoryPool<T>* current_head = __atomic_load_n(&head_pool_, __ATOMIC_RELAXED);

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

template <typename T> std::uintptr_t* ExpandablePoolAllocator<T>::get_flag_for_object(T* obj) {
    using SlotType = Slot<T>;

    auto* storage_ptr = reinterpret_cast<std::aligned_storage_t<sizeof(T), alignof(T)>*>(obj);

    auto* slot = reinterpret_cast<SlotType*>(reinterpret_cast<char*>(storage_ptr) - offsetof(SlotType, storage));

    return &slot->flag;
}

template <typename T>
AllocatorBehaviourStatistics ExpandablePoolAllocator<T>::get_behaviour_statistics() const {
    AllocatorBehaviourStatistics stats;

    stats.total_allocations = total_allocations_.value.load(std::memory_order_relaxed);
    stats.fast_path_allocations = fast_path_allocations_.value.load(std::memory_order_relaxed);
    stats.slow_path_allocations = slow_path_allocations_.value.load(std::memory_order_relaxed);
    stats.expansion_events = expansion_events_.value.load(std::memory_order_relaxed);
    stats.failed_allocations = failed_allocations_.value.load(std::memory_order_relaxed);

    // Count pools
    uint64_t pool_count = 0;
    for (auto* p = head_pool_; p != nullptr; p = p->get_next_pool()) {
        pool_count++;
    }
    stats.pool_count = pool_count;

    // Allocate per-pool array
    stats.per_pool_allocation_counts.count = pool_count;
    if (pool_count > 0) {
        stats.per_pool_allocation_counts.counts =
            new uint64_t[pool_count]();

        uint64_t i = 0;
        for (auto* p = head_pool_; p != nullptr; p = p->get_next_pool()) {
            stats.per_pool_allocation_counts.counts[i++] = p->get_allocation_count();
        }
    }

    return stats;
}

} // namespace pubsub_itc_fw
