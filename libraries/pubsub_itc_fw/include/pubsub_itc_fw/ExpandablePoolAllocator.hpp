#pragma once

#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <new>

namespace pubsub_itc_fw {

template <typename T>
class ExpandablePoolAllocator final {
public:
    ExpandablePoolAllocator(LoggerInterface& logger, const std::string& pool_name,
                            int objects_per_pool, int initial_pools, int max_pools,
                            std::function<void(void*, int)> handler_for_pool_exhausted,
                            std::function<void(void*, void*)> handler_for_invalid_free,
                            std::function<void(void*)> handler_for_huge_pages_error,
                            UseHugePagesFlag use_huge_pages_flag);

    ~ExpandablePoolAllocator() = default;

    [[nodiscard]] T* allocate();
    void deallocate(T* obj);
    PoolStatistics get_pool_statistics() const;

private:
    FixedSizeMemoryPool<T>* add_pool_to_chain();

    std::string pool_name_;
    LoggerInterface& logger_;
    int objects_per_pool_;
    int initial_pools_;
    int max_pools_;
    UseHugePagesFlag use_huge_pages_flag_;
    std::function<void(void*, int)> handler_for_pool_exhausted_;
    std::function<void(void*, void*)> handler_for_invalid_free_;
    std::function<void(void*)> handler_for_huge_pages_error_;

    FixedSizeMemoryPool<T>* head_pool_{nullptr};
    FixedSizeMemoryPool<T>* current_pool_ptr_{nullptr};
    mutable std::mutex expansion_mutex_;
    std::vector<std::unique_ptr<FixedSizeMemoryPool<T>>> cleanup_list_;

    ExpandablePoolAllocator(const ExpandablePoolAllocator&) = delete;
    ExpandablePoolAllocator& operator=(const ExpandablePoolAllocator&) = delete;
};

template <typename T>
ExpandablePoolAllocator<T>::ExpandablePoolAllocator(
    LoggerInterface& logger, const std::string& pool_name, int objects_per_pool,
    int initial_pools, int max_pools, std::function<void(void*, int)> h_exhausted,
    std::function<void(void*, void*)> h_invalid, std::function<void(void*)> h_huge,
    UseHugePagesFlag use_huge_pages_flag)
    : pool_name_(pool_name), logger_(logger), objects_per_pool_(objects_per_pool),
      initial_pools_(initial_pools), max_pools_(max_pools), use_huge_pages_flag_(use_huge_pages_flag),
      handler_for_pool_exhausted_(std::move(h_exhausted)), handler_for_invalid_free_(std::move(h_invalid)),
      handler_for_huge_pages_error_(std::move(h_huge)) {
    for (int i = 0; i < initial_pools_; ++i) add_pool_to_chain();
}

template <typename T>
FixedSizeMemoryPool<T>* ExpandablePoolAllocator<T>::add_pool_to_chain() {
    auto new_pool = std::make_unique<FixedSizeMemoryPool<T>>(
        objects_per_pool_, use_huge_pages_flag_,
        [this](void* addr, size_t) { if (handler_for_huge_pages_error_) handler_for_huge_pages_error_(addr); });

    FixedSizeMemoryPool<T>* pool_ptr = new_pool.get();
    FixedSizeMemoryPool<T>* current_head = __atomic_load_n(&head_pool_, __ATOMIC_RELAXED);

    if (!current_head) {
        __atomic_store_n(&head_pool_, pool_ptr, __ATOMIC_RELEASE);
    } else {
        FixedSizeMemoryPool<T>* tail = current_head;
        while (tail->get_next_pool()) tail = tail->get_next_pool();
        tail->set_next_pool(pool_ptr); // Link established FIRST
    }

    cleanup_list_.push_back(std::move(new_pool));
    __atomic_store_n(&current_pool_ptr_, pool_ptr, __ATOMIC_RELEASE); // Update current LAST
    return pool_ptr;
}

template <typename T>
T* ExpandablePoolAllocator<T>::allocate() {
    T* raw_mem = nullptr;
    FixedSizeMemoryPool<T>* pool = __atomic_load_n(&current_pool_ptr_, __ATOMIC_ACQUIRE);
    if (pool) raw_mem = pool->allocate();

    if (!raw_mem) {
        std::lock_guard<std::mutex> lock(expansion_mutex_);
        pool = __atomic_load_n(&current_pool_ptr_, __ATOMIC_ACQUIRE);
        raw_mem = pool->allocate();

        if (!raw_mem) {
            FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
            while (traverse) {
                raw_mem = traverse->allocate();
                if (raw_mem) {
                    __atomic_store_n(&current_pool_ptr_, traverse, __ATOMIC_RELEASE);
                    break;
                }
                traverse = traverse->get_next_pool();
            }
        }

        if (!raw_mem && cleanup_list_.size() < static_cast<size_t>(max_pools_)) {
            FixedSizeMemoryPool<T>* new_pool = add_pool_to_chain();
            raw_mem = new_pool->allocate();
            if (handler_for_pool_exhausted_) handler_for_pool_exhausted_(nullptr, objects_per_pool_);
        }
    }

    return raw_mem ? ::new (static_cast<void*>(raw_mem)) T() : nullptr; // Placement New
}

template <typename T>
void ExpandablePoolAllocator<T>::deallocate(T* obj) {
    if (!obj) return;
    obj->~T(); // Explicit Destructor

    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
    while (traverse) {
        if (traverse->contains(obj)) {
            traverse->deallocate(obj);
            return;
        }
        traverse = traverse->get_next_pool();
    }
    if (handler_for_invalid_free_) handler_for_invalid_free_(nullptr, obj);
}

template <typename T>
PoolStatistics ExpandablePoolAllocator<T>::get_pool_statistics() const {
    PoolStatistics stats;
    stats.pool_name_ = pool_name_;
    stats.object_size_ = sizeof(T);
    stats.number_of_objects_per_pool_ = objects_per_pool_;
    stats.use_huge_pages_flag_ = use_huge_pages_flag_;
    FixedSizeMemoryPool<T>* traverse = __atomic_load_n(&head_pool_, __ATOMIC_ACQUIRE);
    while (traverse) {
        stats.number_of_pools_++;
        int available = traverse->get_number_of_available_objects();
        stats.number_of_allocated_objects_ += (objects_per_pool_ - available);
        stats.number_of_objects_available_ += available;
        if (traverse->is_full()) stats.number_of_full_pools_++;
        if (traverse->uses_huge_pages()) {
            stats.number_of_huge_page_pools_++;
            stats.huge_page_size_ = traverse->get_huge_page_size();
        }
        traverse = traverse->get_next_pool();
    }
    return stats;
}

} // namespace pubsub_itc_fw
