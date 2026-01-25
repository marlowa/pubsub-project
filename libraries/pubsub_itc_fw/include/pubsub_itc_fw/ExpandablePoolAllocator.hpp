#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

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
 *    - Pools linked via next_pool_ pointers (singly-linked list)
 *    - head_pool_ points to first pool in chain
 *    - current_pool_ptr_ points to most recently active pool (optimization)
 *    - cleanup_list_ holds unique_ptrs for RAII cleanup
 *
 * EXPANSION STRATEGY
 *    - Starts with initial_pools pre-allocated
 *    - Expands on-demand when all pools exhausted
 *    - No hard limit - expands as long as OS can provide memory
 *    - expansion_threshold_hint is for monitoring/alerts only
 *
 * OBJECT LIFETIME MANAGEMENT
 *    - allocate() returns CONSTRUCTED objects (placement new applied)
 *    - deallocate() DESTRUCTS objects before returning memory to pool
 *    - FixedSizeMemoryPool deals only with raw memory
 *    - Objects are destructed immediately before their memory is returned to
 *      the underlying pool during deallocate().
 *    - Callers must not use delete on objects returned by allocate().
 *
 * DEALLOCATION STRATEGY
 *    - Linear search through chain to find owning pool
 *    - O(N) where N is number of pools, but typically N is small
 *    - Acceptable for exchange environments where deallocation is off critical path
 *
 * POOL OWNERSHIP
 *    - All FixedSizeMemoryPool instances are owned by this allocator.
 *    - Pools are added to the chain but never removed during the allocator's
 *      lifetime. This ensures that traversal of the pool chain is always safe.
 *    - All pools are destroyed together when the allocator itself is destroyed.
 *
 * THREAD SAFETY:
 *    - allocate() is thread-safe (fast path lock-free, slow path mutex)
 *    - deallocate() is thread-safe (traversal uses atomic loads)
 *    - get_pool_statistics() is thread-safe (snapshot via atomic loads)
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

/**
 * @class ExpandablePoolAllocator
 * @brief Thread-safe allocator managing a chain of lock-free memory pools.
 *
 * This allocator provides a hybrid lock-free/locked allocation strategy:
 * - Fast path: Lock-free allocation from a cached "current" pool
 * - Slow path: Mutex-protected chain search and automatic expansion
 *
 * The allocator manages object lifetime (construction/destruction) while
 * delegating raw memory management to FixedSizeMemoryPool instances.
 *
 * @tparam T The type of object to allocate
 *
 * Thread Safety: Fully thread-safe for concurrent allocate() and deallocate()
 *                operations from multiple threads.
 */
template <typename T>
class ExpandablePoolAllocator final {
public:
    /**
     * @brief Destructor - cleans up all pools via cleanup_list_.
     */
    ~ExpandablePoolAllocator() = default;

    /**
     * @brief Constructs an expandable pool allocator.
     *
     * @param[in] logger Logging interface for system events
     * @param[in] pool_name Unique name for the pool (for statistics/logging)
     * @param[in] objects_per_pool Capacity of each individual pool
     * @param[in] initial_pools Number of pools to pre-allocate at construction
     * @param[in] expansion_threshold_hint Soft limit for monitoring (not enforced)
     * @param[in] handler_for_pool_exhausted Callback invoked when new pool added
     * @param[in] handler_for_invalid_free Callback for invalid deallocate() calls
     * @param[in] handler_for_huge_pages_error Callback if huge page allocation fails
     * @param[in] use_huge_pages_flag Whether to attempt 2MB huge page allocation
     *
     * @note The allocator will expand beyond expansion_threshold_hint if needed.
     *       It's purely for monitoring/alerting purposes.
     */
    ExpandablePoolAllocator(LoggerInterface& logger, const std::string& pool_name, int objects_per_pool, int initial_pools, int expansion_threshold_hint,
                            std::function<void(void*, int)> handler_for_pool_exhausted, std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error, UseHugePagesFlag use_huge_pages_flag);

    ExpandablePoolAllocator(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator& operator=(const ExpandablePoolAllocator&) = delete;

    /**
     * @brief Allocates and constructs an object.
     *
     * Fast path: Attempts lock-free allocation from current_pool_ptr_.
     * Slow path: Under mutex, searches all pools, expands if necessary.
     *
     * @return Pointer to newly constructed object, or nullptr if OS memory exhausted
     *
     * @note Returns nullptr only in catastrophic memory exhaustion scenarios.
     *       Under normal operation, the allocator will expand to satisfy requests.
     */
    [[nodiscard]] T* allocate();

    /**
     * @brief Destructs and deallocates an object.
     *
     * Searches the pool chain to find the owning pool and returns the memory.
     * Invokes handler_for_invalid_free if pointer doesn't belong to any pool.
     *
     * @param[in] obj Pointer to object to deallocate (nullptr is safe)
     *
     * @note Object is destructed before memory is returned to pool.
     */
    void deallocate(T* obj);

    /**
     * @brief Collects snapshot of current allocation state.
     *
     * @return PoolStatistics containing pool count, allocation stats, etc.
     *
     * @note This is a snapshot; values may be stale in multithreaded environment.
     */
    [[nodiscard]] PoolStatistics get_pool_statistics() const;

private:
    /**
     * @brief Creates and links a new pool to the chain.
     *
     * @return Pointer to the newly created pool
     *
     * @note Must be called under expansion_mutex_.
     * @note Does NOT update current_pool_ptr_ (caller's responsibility).
     */
    FixedSizeMemoryPool<T>* add_pool_to_chain();

    std::string pool_name_;                                     ///< Name for logging/statistics
    LoggerInterface& logger_;                                   ///< Logging interface
    int objects_per_pool_;                                      ///< Capacity of each pool
    int initial_pools_;                                         ///< Number of pools pre-allocated
    int expansion_threshold_hint_;                              ///< Soft limit for monitoring
    UseHugePagesFlag use_huge_pages_flag_;                      ///< Huge page configuration
    std::function<void(void*, int)> handler_for_pool_exhausted_; ///< New pool callback
    std::function<void(void*, void*)> handler_for_invalid_free_; ///< Invalid free callback
    std::function<void(void*)> handler_for_huge_pages_error_;    ///< Huge page error callback

    FixedSizeMemoryPool<T>* head_pool_{nullptr};                ///< First pool in chain
    FixedSizeMemoryPool<T>* current_pool_ptr_{nullptr};         ///< Cached active pool (atomic)
    mutable std::mutex expansion_mutex_;                        ///< Protects chain modification
    std::vector<std::unique_ptr<FixedSizeMemoryPool<T>>> cleanup_list_; ///< RAII ownership
};

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(LoggerInterface& logger, const std::string& pool_name, int objects_per_pool, int initial_pools,
                                                   int expansion_threshold_hint, std::function<void(void*, int)> h_exhausted,
                                                   std::function<void(void*, void*)> h_invalid, std::function<void(void*)> h_huge,
                                                   UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(pool_name), logger_(logger), objects_per_pool_(objects_per_pool), initial_pools_(initial_pools),
      expansion_threshold_hint_(expansion_threshold_hint), use_huge_pages_flag_(use_huge_pages_flag), handler_for_pool_exhausted_(std::move(h_exhausted)),
      handler_for_invalid_free_(std::move(h_invalid)), handler_for_huge_pages_error_(std::move(h_huge)) {
    for (int i = 0; i < initial_pools_; ++i) {
        add_pool_to_chain();
    }
}

template <typename T>
T* ExpandablePoolAllocator<T>::allocate() {
    // Fast path: try current pool without lock
    T* raw_mem = nullptr;
    FixedSizeMemoryPool<T>* pool = __atomic_load_n(&current_pool_ptr_, __ATOMIC_ACQUIRE);
    
    if (pool != nullptr) {
        raw_mem = pool->allocate();
        if (raw_mem != nullptr) {
            return ::new (static_cast<void*>(raw_mem)) T();
        }
    }

    // Slow path: need to search or expand
    std::lock_guard<std::mutex> lock(expansion_mutex_);
    
    // Reload head_pool_ inside mutex to see any pools added by other threads
    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);

    // Search ALL pools in the chain for available memory
    while (traverse != nullptr) {
        raw_mem = traverse->allocate();
        if (raw_mem != nullptr) {
            // Update current_pool_ptr to this pool for future fast-path attempts
            __atomic_store_n(&current_pool_ptr_, traverse, __ATOMIC_RELEASE);
            return ::new (static_cast<void*>(raw_mem)) T();
        }
        traverse = traverse->get_next_pool();
    }

    // No pools have space, expand the chain
    FixedSizeMemoryPool<T>* new_pool = add_pool_to_chain();
    if (new_pool != nullptr) {
        // CRITICAL: Allocate while still holding lock to prevent fast-path races
        raw_mem = new_pool->allocate();
        
        if (raw_mem != nullptr) {
            // Only NOW make this pool visible to fast-path threads
            // After we've already taken our object from it
            __atomic_store_n(&current_pool_ptr_, new_pool, __ATOMIC_RELEASE);
            
            if (handler_for_pool_exhausted_ != nullptr) {
                handler_for_pool_exhausted_(nullptr, objects_per_pool_);
            }
            
            return ::new (static_cast<void*>(raw_mem)) T();
        }
    }
    
    return nullptr;
}

template <typename T>
void ExpandablePoolAllocator<T>::deallocate(T* obj) {
    if (obj == nullptr) {
        return;
    }
    
    // Destruct the object before returning memory
    obj->~T();

    // Linear search through chain to find owning pool
    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
    while (traverse != nullptr) {
        if (traverse->contains(obj)) {
            traverse->deallocate(obj);
            return;
        }
        traverse = traverse->get_next_pool();
    }
    
    // Pointer doesn't belong to any pool in our chain
    if (handler_for_invalid_free_ != nullptr) {
        handler_for_invalid_free_(nullptr, obj);
    }
}

template <typename T>
FixedSizeMemoryPool<T>* ExpandablePoolAllocator<T>::add_pool_to_chain() {
    auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(objects_per_pool_, use_huge_pages_flag_, [this](void* addr, size_t) {
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
    
    // DO NOT update current_pool_ptr_ here - let the caller do it after allocating
    // This prevents fast-path races when objects_per_pool is small
    
    return pool_ptr;
}

template <typename T>
PoolStatistics ExpandablePoolAllocator<T>::get_pool_statistics() const {
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

} // namespace pubsub_itc_fw
