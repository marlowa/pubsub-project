#pragma once

// Disable pedantic warnings for this file: we use __int128 (a GCC extension) for
// lock-free 16-byte atomic operations via CMPXCHG16B. This is necessary for true
// lock-free performance without libatomic dependency. __int128 is supported by
// GCC 4.6+, Clang 3.0+, and is the standard approach for 128-bit atomics on x86-64.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

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
        HeadPtr current = load_head();
        return current.ptr == nullptr;
    }

    [[nodiscard]] int get_number_of_available_objects() const {
        HeadPtr current = load_head();
        FreeListNode* node = current.ptr;
        int count = 0;
        while (node != nullptr) {
            count++;
            node = node->next;
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

    // Use __int128 for lock-free operations with __sync_val_compare_and_swap
    using atomic128_t = __int128;
    
    static_assert(sizeof(HeadPtr) == sizeof(atomic128_t), "HeadPtr must be 128 bits");
    static_assert(sizeof(HeadPtr) == 16, "HeadPtr must be 16 bytes");
    static_assert(alignof(HeadPtr) == 16, "HeadPtr must be 16-byte aligned");

    // Conversion helpers
    static inline atomic128_t to_int128(const HeadPtr& hp) {
        atomic128_t result;
        __builtin_memcpy(&result, &hp, sizeof(HeadPtr));
        return result;
    }

    static inline HeadPtr from_int128(atomic128_t val) {
        HeadPtr result;
        __builtin_memcpy(&result, &val, sizeof(HeadPtr));
        return result;
    }

    // Lock-free atomic operations using __sync_val_compare_and_swap
    HeadPtr load_head() const {
        // Simple volatile read is sufficient for load with acquire semantics via the barrier
        atomic128_t current = *reinterpret_cast<const volatile atomic128_t*>(&free_list_head_ptr_);
        return from_int128(current);
    }

    void store_head(const HeadPtr& value) {
        atomic128_t desired = to_int128(value);
        *reinterpret_cast<volatile atomic128_t*>(&free_list_head_ptr_) = desired;
        __sync_synchronize(); // Full memory barrier for release semantics
    }

    bool compare_exchange_weak(HeadPtr& expected, const HeadPtr& desired) {
        atomic128_t exp = to_int128(expected);
        atomic128_t des = to_int128(desired);
        
        atomic128_t prev = __sync_val_compare_and_swap(
            reinterpret_cast<volatile atomic128_t*>(&free_list_head_ptr_),
            exp,
            des
        );
        
        if (prev == exp) {
            return true;
        } else {
            expected = from_int128(prev);
            return false;
        }
    }

    void push_node_to_free_list(T* node_to_push);
    T* pop_node_from_free_list();

    int objects_per_pool_;
    UseHugePagesFlag use_huge_pages_flag_;
    size_t huge_page_size_{0};
    void* pool_memory_{nullptr};
    size_t total_pool_size_{0};

    // 16-byte aligned storage for lock-free CAS operations
    alignas(16) HeadPtr free_list_head_ptr_;
};



template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool,
                                            UseHugePagesFlag use_huge_pages_flag,
                                            std::function<void(void*, size_t)> handler_for_huge_pages_error)
    : objects_per_pool_(objects_per_pool),
      use_huge_pages_flag_(use_huge_pages_flag) {
    
    static_assert(sizeof(T) >= sizeof(FreeListNode), "T is too small for intrusive linking");
    
    // Verify CMPXCHG16B support at compile time
    #if !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
        #error "Compiler does not support 16-byte compare-and-swap. Compile with -mcx16"
    #endif

    total_pool_size_ = objects_per_pool_ * sizeof(T);
    
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
        mmap_flags |= MAP_HUGETLB;
        huge_page_size_ = 2 * 1024 * 1024; 
        total_pool_size_ = ((total_pool_size_ + huge_page_size_ - 1) / huge_page_size_) * huge_page_size_;
    }

    pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);

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
    
    store_head({first_node, 0});
}

template <typename T>
FixedSizeMemoryPool<T>::~FixedSizeMemoryPool() {
    if (pool_memory_ != nullptr && pool_memory_ != MAP_FAILED) {
        munmap(pool_memory_, total_pool_size_);
    }
}

template <typename T>
T* FixedSizeMemoryPool<T>::allocate() {
    T* raw_ptr = pop_node_from_free_list();
    if (raw_ptr != nullptr) {
        // Construct the object using placement new
        new (raw_ptr) T();
    }
    return raw_ptr;
}

template <typename T>
void FixedSizeMemoryPool<T>::deallocate(T* node_to_push) {
    if (!contains(node_to_push)) {
        throw PreconditionAssertion("Pointer does not belong to this pool", __FILE__, __LINE__);
    }
    
    // Explicitly destruct the object before returning memory to the pool
    node_to_push->~T();
    
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
    
    while (old_head.ptr != nullptr) {
        HeadPtr next_head;
        next_head.ptr = old_head.ptr->next; 
        next_head.counter = old_head.counter + 1;

        if (compare_exchange_weak(old_head, next_head)) {
            return reinterpret_cast<T*>(old_head.ptr);
        }
    }
    return nullptr;
}

} // namespace pubsub_itc_fw

#pragma GCC diagnostic pop
