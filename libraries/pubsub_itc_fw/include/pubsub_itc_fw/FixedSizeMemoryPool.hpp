#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <new>

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

    // Returns raw memory. The Allocator is responsible for construction.
    T* allocate();
    void deallocate(T* node_to_push);

    [[nodiscard]] bool contains(const T* ptr) const;
    [[nodiscard]] bool is_full() const;
    [[nodiscard]] int get_number_of_available_objects() const;
    [[nodiscard]] bool uses_huge_pages() const;
    [[nodiscard]] size_t get_huge_page_size() const;

    void set_next_pool(FixedSizeMemoryPool<T>* next) { 
        __atomic_store_n(&next_pool_, next, __ATOMIC_RELEASE);
    }
    
    FixedSizeMemoryPool<T>* get_next_pool() const { 
        return __atomic_load_n(&next_pool_, __ATOMIC_ACQUIRE);
    }

private:
    struct FreeListNode {
        FreeListNode* next;
    };

    struct alignas(16) HeadPtr {
        FreeListNode* ptr;
        uint64_t counter;
    };

    void push_node_to_free_list(T* node_to_push);
    T* pop_node_from_free_list();

    inline HeadPtr load_head() const noexcept {
        unsigned __int128 val;
        __atomic_load(&head_raw_, &val, __ATOMIC_ACQUIRE);
        return *reinterpret_cast<const HeadPtr*>(&val);
    }

    inline bool compare_exchange_weak(HeadPtr& expected, HeadPtr desired) noexcept {
        return __atomic_compare_exchange(
            &head_raw_,
            reinterpret_cast<unsigned __int128*>(&expected),
            reinterpret_cast<unsigned __int128*>(&desired),
            true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    int objects_per_pool_;
    UseHugePagesFlag use_huge_pages_flag_;
    size_t total_pool_size_;
    void* pool_memory_;
    
    alignas(16) mutable unsigned __int128 head_raw_{0};
    FixedSizeMemoryPool<T>* next_pool_{nullptr};
};

template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool,
                                            UseHugePagesFlag use_huge_pages_flag,
                                            std::function<void(void*, size_t)> handler_for_huge_pages_error)
    : objects_per_pool_(objects_per_pool),
      use_huge_pages_flag_(use_huge_pages_flag) {
    
#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
    static_assert(false, "Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags.");
#endif
    static_assert(alignof(HeadPtr) == 16, "HeadPtr must be 16-byte aligned.");

    size_t object_size = std::max(sizeof(T), sizeof(FreeListNode));
    total_pool_size_ = static_cast<size_t>(objects_per_pool_) * object_size;

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) flags |= MAP_HUGETLB;

    pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (pool_memory_ == MAP_FAILED) throw std::bad_alloc();

    std::byte* current = static_cast<std::byte*>(pool_memory_);
    for (int i = 0; i < objects_per_pool_; ++i) {
        push_node_to_free_list(reinterpret_cast<T*>(current));
        current += object_size;
    }
}

template <typename T>
FixedSizeMemoryPool<T>::~FixedSizeMemoryPool() {
    if (pool_memory_ != MAP_FAILED) munmap(pool_memory_, total_pool_size_);
}

template <typename T>
T* FixedSizeMemoryPool<T>::allocate() { return pop_node_from_free_list(); }

template <typename T>
void FixedSizeMemoryPool<T>::deallocate(T* node_to_push) {
    if (!node_to_push) return;
    push_node_to_free_list(node_to_push);
}

template <typename T>
bool FixedSizeMemoryPool<T>::contains(const T* ptr) const {
    const std::byte* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
    const std::byte* start = static_cast<const std::byte*>(pool_memory_);
    return (byte_ptr >= start && byte_ptr < start + total_pool_size_);
}

template <typename T>
bool FixedSizeMemoryPool<T>::is_full() const { return load_head().ptr == nullptr; }

template <typename T>
int FixedSizeMemoryPool<T>::get_number_of_available_objects() const {
    int count = 0;
    FreeListNode* current = load_head().ptr;
    while (current) { count++; current = current->next; }
    return count;
}

template <typename T>
bool FixedSizeMemoryPool<T>::uses_huge_pages() const { return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages; }

template <typename T>
size_t FixedSizeMemoryPool<T>::get_huge_page_size() const { return uses_huge_pages() ? 2048 * 1024 : 0; }

template <typename T>
void FixedSizeMemoryPool<T>::push_node_to_free_list(T* node_to_push) {
    FreeListNode* new_node = reinterpret_cast<FreeListNode*>(node_to_push);
    HeadPtr old_head = load_head();
    HeadPtr next_head;
    do {
        new_node->next = old_head.ptr;
        next_head.ptr = new_node;
        next_head.counter = old_head.counter + 1;
    } while (!compare_exchange_weak(old_head, next_head));
}

template <typename T>
T* FixedSizeMemoryPool<T>::pop_node_from_free_list() {
    HeadPtr old_head = load_head();
    HeadPtr next_head;
    while (old_head.ptr != nullptr) {
        next_head.ptr = old_head.ptr->next;
        next_head.counter = old_head.counter + 1;
        if (compare_exchange_weak(old_head, next_head)) return reinterpret_cast<T*>(old_head.ptr);
    }
    return nullptr;
}

} // namespace pubsub_itc_fw
#pragma GCC diagnostic pop
