#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <string>
#include <algorithm>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

namespace pubsub_itc_fw {

template <typename T>
class FixedSizeMemoryPool final {
public:
    FixedSizeMemoryPool(int objects_per_pool,
                        UseHugePagesFlag use_huge_pages_flag,
                        std::function<void(void*, size_t)> handler_for_huge_pages_error);

    ~FixedSizeMemoryPool();

    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;

    T* allocate();
    void deallocate(T* node_to_push);

    [[nodiscard]] bool contains(const T* ptr) const;
    
    [[nodiscard]] bool is_full() const {
        return free_list_head_ptr_.load(std::memory_order_acquire).ptr == nullptr;
    }

    [[nodiscard]] int get_number_of_available_objects() const {
        int count = 0;
        HeadPtr current_head = free_list_head_ptr_.load(std::memory_order_acquire);
        FreeListNode* current = current_head.ptr;
        while (current != nullptr) {
            count++;
            current = current->next;
        }
        return count;
    }

    [[nodiscard]] bool uses_huge_pages() const { 
        return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages; 
    }
    
    [[nodiscard]] size_t get_huge_page_size() const { return huge_page_size_; }

private:
    struct FreeListNode {
        FreeListNode* next;
    };

    struct alignas(16) HeadPtr {
        FreeListNode* ptr;
        uintptr_t counter;

        bool operator==(const HeadPtr& other) const noexcept {
            return ptr == other.ptr && counter == other.counter;
        }
    };

    void push_node_to_free_list(T* node_to_push);
    T* pop_node_from_free_list();

    int objects_per_pool_;
    UseHugePagesFlag use_huge_pages_flag_;
    size_t huge_page_size_{0};
    void* pool_memory_{nullptr};
    size_t total_pool_size_{0};

    // The generation counter prevents ABA logic errors.
    // The aligned memory ensures the hardware can perform the CAS atomically.
    std::atomic<HeadPtr> free_list_head_ptr_;
};



template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool,
                                            UseHugePagesFlag use_huge_pages_flag,
                                            std::function<void(void*, size_t)> handler_for_huge_pages_error)
    : objects_per_pool_(objects_per_pool),
      use_huge_pages_flag_(use_huge_pages_flag) {
    
    static_assert(sizeof(T) >= sizeof(FreeListNode), "T is too small for intrusive linking");
    
    // Ensure we are not using a fallback "lock-based" atomic which would fail in an exchange
    if (!free_list_head_ptr_.is_lock_free()) {
        throw std::runtime_error("Hardware does not support lock-free 16-byte atomics.");
    }

    total_pool_size_ = objects_per_pool_ * sizeof(T);
    
    // We use mmap even for standard pages to ensure the memory block 
    // is contiguous and has consistent lifecycle properties.
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
        mmap_flags |= MAP_HUGETLB;
        huge_page_size_ = 2 * 1024 * 1024; 
        total_pool_size_ = ((total_pool_size_ + huge_page_size_ - 1) / huge_page_size_) * huge_page_size_;
    }

    pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);

    // Fallback logic if Huge Pages are requested but denied by the OS hardware
    if (pool_memory_ == MAP_FAILED && (mmap_flags & MAP_HUGETLB)) {
        if (handler_for_huge_pages_error) handler_for_huge_pages_error(nullptr, total_pool_size_);
        use_huge_pages_flag_ = UseHugePagesFlag::DoNotUseHugePages;
        mmap_flags &= ~MAP_HUGETLB;
        total_pool_size_ = objects_per_pool_ * sizeof(T);
        pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    }

    if (pool_memory_ == MAP_FAILED) throw std::runtime_error("mmap failed");

    FreeListNode* first_node = nullptr;
    for (int i = objects_per_pool_ - 1; i >= 0; --i) {
        FreeListNode* current = reinterpret_cast<FreeListNode*>(static_cast<std::byte*>(pool_memory_) + (i * sizeof(T)));
        current->next = first_node;
        first_node = current;
    }
    
    free_list_head_ptr_.store({first_node, 0}, std::memory_order_release);
}

template <typename T>
FixedSizeMemoryPool<T>::~FixedSizeMemoryPool() {
    if (pool_memory_ != nullptr && pool_memory_ != MAP_FAILED) {
        munmap(pool_memory_, total_pool_size_);
    }
}

template <typename T>
T* FixedSizeMemoryPool<T>::allocate() {
    return pop_node_from_free_list();
}

template <typename T>
void FixedSizeMemoryPool<T>::deallocate(T* node_to_push) {
    if (!contains(node_to_push)) {
        throw PreconditionAssertion("Pointer does not belong to this pool", __FILE__, __LINE__);
    }
    push_node_to_free_list(node_to_push);
}

template <typename T>
bool FixedSizeMemoryPool<T>::contains(const T* ptr) const {
    const std::byte* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
    const std::byte* start = static_cast<const std::byte*>(pool_memory_);
    return (byte_ptr >= start && byte_ptr < start + total_pool_size_);
}

template <typename T>
void FixedSizeMemoryPool<T>::push_node_to_free_list(T* node_to_push) {
    FreeListNode* new_node = reinterpret_cast<FreeListNode*>(node_to_push);
    HeadPtr old_head = free_list_head_ptr_.load(std::memory_order_relaxed);
    HeadPtr next_head;

    do {
        new_node->next = old_head.ptr;
        next_head.ptr = new_node;
        next_head.counter = old_head.counter + 1;
    } while (!free_list_head_ptr_.compare_exchange_weak(
                old_head,
                next_head,
                std::memory_order_release,
                std::memory_order_relaxed));
}

template <typename T>
T* FixedSizeMemoryPool<T>::pop_node_from_free_list() {
    HeadPtr old_head = free_list_head_ptr_.load(std::memory_order_acquire);
    while (old_head.ptr != nullptr) {
        HeadPtr next_head;
        // Even with standard pages, this dereference is safe because the 
        // whole block is mmapped once and remains valid until the pool is destroyed.
        next_head.ptr = old_head.ptr->next; 
        next_head.counter = old_head.counter + 1;

        if (free_list_head_ptr_.compare_exchange_weak(
                old_head,
                next_head,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return reinterpret_cast<T*>(old_head.ptr);
        }
    }
    return nullptr;
}

} // namespace pubsub_itc_fw
