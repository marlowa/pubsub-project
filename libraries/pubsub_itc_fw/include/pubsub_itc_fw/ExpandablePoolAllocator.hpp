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
    // This is the simplest and safest way to manage a dynamically sized vector in a multi-threaded context.
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
      max_pools_(max_pools),
      use_huge_pages_flag_(use_huge_pages_flag),
      handler_for_pool_exhausted_(std::move(handler_for_pool_exhausted)),
      handler_for_invalid_free_(std::move(handler_for_invalid_free)),
      handler_for_huge_pages_error_(std::move(handler_for_huge_pages_error)) {
    // Pre-allocate the initial pools
    pools_.reserve(max_pools_);
    for (int i = 0; i < initial_pools; ++i) {
        pools_.push_back(std::make_unique<FixedSizeMemoryPool<T>>(
            objects_per_pool, use_huge_pages_flag, 0, 0, handler_for_huge_pages_error, nullptr));
    }
    // Set the initial current pool pointer
    if (!pools_.empty()) {
        current_pool_ptr_.store(pools_[0].get(), std::memory_order_release);
    }
}

template <typename T>
ExpandablePoolAllocator<T>::~ExpandablePoolAllocator() = default;

template <typename T>
T* ExpandablePoolAllocator<T>::allocate() {
    T* obj = nullptr;

    // The main allocation loop is lock-free. Threads race to get an object from the current pool.
    while (true) {
        FixedSizeMemoryPool<T>* pool = current_pool_ptr_.load(std::memory_order_acquire);

        if (pool != nullptr) {
            obj = pool->allocate();
            if (obj != nullptr) {
                return obj; // Successful lock-free allocation
            }
        }

        // If here, the current pool is full or null. This is the expansion path, which is infrequent.
        // We use a mutex to safely modify the shared pools_ vector.
        std::lock_guard<std::mutex> lock(expansion_mutex_);

        // Re-check if the pool is still full after acquiring the lock.
        // Another thread might have already performed the expansion.
        pool = current_pool_ptr_.load(std::memory_order_acquire);
        if (pool != nullptr) {
            obj = pool->allocate();
            if (obj != nullptr) {
                return obj;
            }
        }

        // If still full, and we haven't reached the max pool limit, expand.
        if (pools_.size() < static_cast<size_t>(max_pools_)) {
            auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(
                objects_per_pool_,
                use_huge_pages_flag_,
                0, // huge_page_size
                0, // pool_size_rounded_to_huge_page_size
                handler_for_huge_pages_error_,
                nullptr);

            // Add the new pool to the vector and update the atomic pointer.
            pools_.push_back(std::move(new_pool));
            current_pool_ptr_.store(pools_.back().get(), std::memory_order_release);

            // Invoke the callback for expansion
            if (handler_for_pool_exhausted_) {
                handler_for_pool_exhausted_(nullptr, objects_per_pool_);
            }

            // Attempt to allocate from the newly created pool
            obj = pools_.back()->allocate();
            if (obj != nullptr) {
                return obj; // Successful allocation from the new pool
            }
        } else {
            // Max pools reached, and no object was available. Return nullptr.
            return nullptr;
        }
    }
}

template <typename T>
void ExpandablePoolAllocator<T>::deallocate(T* obj) {
    if (obj == nullptr) {
        return;
    }

    // Try-catch block to handle the PreconditionAssertion thrown by FixedSizeMemoryPool::deallocate
    try {
        for (auto& pool : pools_) {
            if (pool->contains(obj)) {
                pool->deallocate(obj);
                return;
            }
        }
    } catch (const pubsub_itc_fw::PreconditionAssertion& e) {
        // The exception means the pointer was invalid.
    }

    // If the loop completes, the object does not belong to any pool managed by this allocator.
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
        stats.number_of_allocated_objects_ += pool->get_number_of_allocated_objects();
        stats.number_of_objects_available_ += pool->get_number_of_available_objects();
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
