#pragma once

#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <mutex>

namespace pubsub_itc_fw {

/**
 * @class ExpandablePoolAllocator
 * @brief An expandable, thread-safe memory pool allocator.
 *
 * This allocator is primarily lock-free for high-frequency allocation and
 * deallocation from existing pools. It uses a hybrid approach, employing a
 * single mutex to safely handle the infrequent and non-performant operation
 * of expanding the pool chain.
 *
 * @tparam T The type of object to be allocated.
 */
template <typename T>
class ExpandablePoolAllocator final {
public:
    /**
     * @brief Constructor for the expandable pool allocator.
     */
    ExpandablePoolAllocator(LoggerInterface& logger,
                            const std::string& pool_name,
                            int objects_per_pool,
                            int initial_pools,
                            int max_pools,
                            std::function<void(void*, int)> handler_for_pool_exhausted,
                            std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error,
                            UseHugePagesFlag use_huge_pages_flag);

    ~ExpandablePoolAllocator();

    /**
     * @brief Allocates an object from the pool.
     *
     * This method attempts a lock-free allocation from the current pool. If
     * the current pool is full, it acquires a lock to safely expand the pool
     * chain. This is a thread-safe operation.
     *
     * @return A pointer to the newly allocated object, or `nullptr` if a new
     * pool cannot be created (max limit reached).
     */
    [[nodiscard]] T* allocate();

    /**
     * @brief Deallocates an object back to the pool.
     *
     * This method is thread-safe. It uses a try-catch block to gracefully
     * handle invalid pointers without throwing an exception.
     *
     * @param obj The pointer to the object to deallocate.
     */
    void deallocate(T* obj);

    /**
     * @brief Provides a snapshot of the current pool statistics.
     *
     * This method is thread-safe.
     *
     * @return A `PoolStatistics` struct containing the current state.
     */
    PoolStatistics get_pool_statistics() const;

private:
    std::string pool_name_;
    LoggerInterface& logger_;
    int objects_per_pool_;
    int initial_pools_;
    int max_pools_;
    UseHugePagesFlag use_huge_pages_flag_;
    std::function<void(void*, int)> handler_for_pool_exhausted_;
    std::function<void(void*, void*)> handler_for_invalid_free_;
    std::function<void(void*)> handler_for_huge_pages_error_;

    // To prevent the object from being copyable or moveable.
    ExpandablePoolAllocator(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator& operator=(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator(ExpandablePoolAllocator&&) = delete;
    ExpandablePoolAllocator& operator=(ExpandablePoolAllocator&&) = delete;

    // The primary storage for the memory pools.
    std::vector<std::unique_ptr<FixedSizeMemoryPool<T>>> pools_;

    // The current pool for allocations, atomically managed for lock-free access.
    std::atomic<FixedSizeMemoryPool<T>*> current_pool_ptr_{nullptr};

    // A single mutex to protect the pools_ vector when a new pool is added.
    std::mutex expansion_mutex_;
};

// --- ExpandablePoolAllocator Method Implementations ---

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(
    LoggerInterface& logger,
    const std::string& pool_name,
    int objects_per_pool,
    int initial_pools,
    int max_pools,
    std::function<void(void*, int)> handler_for_pool_exhausted,
    std::function<void(void*, void*)> handler_for_invalid_free,
    std::function<void(void*)> handler_for_huge_pages_error,
    UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(pool_name),
      logger_(logger),
      objects_per_pool_(objects_per_pool),
      initial_pools_(initial_pools),
      max_pools_(max_pools),
      use_huge_pages_flag_(use_huge_pages_flag),
      handler_for_pool_exhausted_(std::move(handler_for_pool_exhausted)),
      handler_for_invalid_free_(std::move(handler_for_invalid_free)),
      handler_for_huge_pages_error_(std::move(handler_for_huge_pages_error)) {
    
    pools_.reserve(max_pools_);

    for (int i = 0; i < initial_pools_; ++i) {
        // Corrected constructor call: Passing only the 3 required arguments
        // and using a lambda to bridge the (void*, size_t) pool callback 
        // to the (void*) allocator callback.
        pools_.push_back(std::make_unique<FixedSizeMemoryPool<T>>(
            objects_per_pool_,
            use_huge_pages_flag_,
            [this](void* addr, size_t /*size*/) {
                if (this->handler_for_huge_pages_error_) {
                    this->handler_for_huge_pages_error_(addr);
                }
            }
        ));
    }

    if (!pools_.empty()) {
        current_pool_ptr_.store(pools_[0].get(), std::memory_order_release);
    }
}

template <typename T>
ExpandablePoolAllocator<T>::~ExpandablePoolAllocator() = default;

template <typename T>
T* ExpandablePoolAllocator<T>::allocate() {
    T* obj = nullptr;

    // 1. Fast Path: Atomic load with acquire semantics
    FixedSizeMemoryPool<T>* pool = current_pool_ptr_.load(std::memory_order_acquire);
    if (pool) {
        obj = pool->allocate();
        if (obj) return obj;
    }

    // 2. Slow Path: Contention/Expansion
    std::lock_guard<std::mutex> lock(expansion_mutex_);

    // RE-CHECK: Another thread might have expanded while we waited for the lock
    pool = current_pool_ptr_.load(std::memory_order_acquire);
    if (pool) {
        obj = pool->allocate();
        if (obj) return obj;
    }

    // 3. Attempt Expansion
    if (pools_.size() < static_cast<size_t>(max_pools_)) {
        auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(
            objects_per_pool_, use_huge_pages_flag_,
            [this](void* addr, size_t /*size*/) {
                if (this->handler_for_huge_pages_error_) {
                    this->handler_for_huge_pages_error_(addr);
                }
            }
        );

        FixedSizeMemoryPool<T>* pool_ptr = new_pool.get();
        pools_.push_back(std::move(new_pool));
        
        // Critical: Store the new pool pointer before performing the callback/allocation
        current_pool_ptr_.store(pool_ptr, std::memory_order_release);

        if (handler_for_pool_exhausted_) {
            handler_for_pool_exhausted_(nullptr, objects_per_pool_);
        }

        // Return from the pool we JUST created
        return pool_ptr->allocate();
    }

    // 4. Final Fallback: Exhaustive search
    // In extreme contention, a deallocate() might have happened in an older pool
    // while we were fighting for the mutex.
    for (const auto& p : pools_) {
        obj = p->allocate();
        if (obj) return obj;
    }

    return nullptr;
}

template <typename T>
void ExpandablePoolAllocator<T>::deallocate(T* obj) {
    if (obj == nullptr) {
        return;
    }

    try {
        // Linear scan of pools to find which one owns this pointer.
        // In exchange environments, this is acceptable because deallocations
        // are often off the critical path or occur in predictable patterns.
        for (auto& pool : pools_) {
            if (pool->contains(obj)) {
                pool->deallocate(obj);
                return;
            }
        }
    } catch (const pubsub_itc_fw::PreconditionAssertion& e) {
        // Logic error: pool claims to contain ptr but failed to deallocate.
    }

    if (handler_for_invalid_free_) {
        handler_for_invalid_free_(nullptr, obj);
    }
}

template <typename T>
PoolStatistics ExpandablePoolAllocator<T>::get_pool_statistics() const {
    PoolStatistics stats;
    stats.pool_name_ = pool_name_;
    stats.object_size_ = sizeof(T);
    stats.number_of_objects_per_pool_ = objects_per_pool_;
    stats.use_huge_pages_flag_ = use_huge_pages_flag_;

    for (const auto& pool : pools_) {
        stats.number_of_pools_++;
            
        // These methods rely on the public interface of our new FixedSizeMemoryPool.
        int available = pool->get_number_of_available_objects();
        stats.number_of_allocated_objects_ += (objects_per_pool_ - available);
        stats.number_of_objects_available_ += available;
    
        if (pool->is_full()) {
            stats.number_of_full_pools_++;
        }
            
        if (pool->uses_huge_pages()) {
            stats.number_of_huge_page_pools_++;
            stats.huge_page_size_ = pool->get_huge_page_size();
        }
    }

    return stats;
}

} // namespace pubsub_itc_fw
