#pragma once

// Disable pedantic warnings for this file: we use __int128 (a GCC extension) for
// lock-free 16-byte atomic operations via CMPXCHG16B. This is necessary for true
// lock-free performance without libatomic dependency. __int128 is supported by
// GCC 4.6+, Clang 3.0+, and is the standard approach for 128-bit atomics on x86-64.
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

/*
 * ============================================================================
 * LOCK-FREE MEMORY POOL DESIGN OVERVIEW
 * ============================================================================
 *
 * This memory pool implements a lock-free stack (Treiber stack) for object
 * allocation using the ABA-prevention technique with a tagged pointer.
 *
 * KEY DESIGN ELEMENTS:
 *
 * 1. INTRUSIVE FREE LIST
 *    - Objects in the free list reuse their own memory to store the 'next' pointer
 *    - This requires sizeof(T) >= sizeof(void*), enforced by static_assert
 *    - No separate node allocations needed - zero overhead when object is in use
 *
 * 2. ABA PROBLEM PREVENTION
 *    - Classic ABA scenario: Thread A reads head->X, gets preempted, Thread B pops X
 *      and Y, then pushes X back. Thread A resumes and CAS succeeds incorrectly.
 *    - Solution: Use a 128-bit structure with {pointer, counter}
 *    - Every CAS operation increments the counter, making each pointer+counter pair unique
 *    - Even if the same address is reused, the counter will differ, preventing ABA
 *
 * 3. 128-BIT ATOMIC OPERATIONS
 *    - x86-64 CMPXCHG16B instruction requires:
 *      * -mcx16 compiler flag
 *      * 16-byte alignment of data (enforced by alignas(16))
 *      * __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16 defined at compile time
 *    - We use unsigned __int128 as the underlying storage type
 *    - GCC built-in __atomic_compare_exchange compiles directly to CMPXCHG16B
 *    - NO libatomic dependency - these are true compiler intrinsics
 *
 * 4. WHY NOT std::atomic<HeadPtr>?
 *    - std::atomic<struct> with runtime is_lock_free() checks link against libatomic
 *    - libatomic returns conservative 'false' even when hardware supports the operation
 *    - Using __int128 with __atomic_* intrinsics bypasses libatomic entirely
 *    - GCC recognizes __int128 and emits CMPXCHG16B directly
 *
 * 5. MEMORY ORDERING
 *    - load_head(): ACQUIRE ordering - ensures we see all prior writes to the node
 *    - compare_exchange_weak(): ACQ_REL on success, ACQUIRE on failure
 *      * RELEASE: our writes to new_node are visible before head update
 *      * ACQUIRE: we see the actual head value if CAS fails
 *
 * 6. MEMORY ALLOCATION STRATEGY
 *    - Uses mmap() for predictable, contiguous allocation
 *    - Optional huge pages (2MB) for reduced TLB pressure on NUMA systems
 *    - Fallback to standard pages if huge pages unavailable
 *    - Memory is never freed until pool destruction (no munmap during operation)
 *
 * 7. THREAD SAFETY GUARANTEES
 *    - Multiple threads can allocate() and deallocate() concurrently
 *    - No locks, no blocking - true wait-free for readers, lock-free for writers
 *    - Safe under all memory orderings on x86-64 TSO (Total Store Order)
 *    - Tested under NUMA with thread pinning for maximum cache contention
 *
 * COMPILER REQUIREMENTS:
 *    - GCC 4.6+ or Clang 3.0+ on x86-64
 *    - Compiler flags: -mcx16 -std=c++17 (or higher)
 *    - Hardware: x86-64 CPU with CMPXCHG16B support (all modern x86-64 CPUs)
 *
 * NUMA CONSIDERATIONS:
 *    - Each pool's memory is allocated on the NUMA node of the creating thread
 *    - For best performance, allocate pools on the same node as worker threads
 *    - Huge pages reduce TLB misses significantly on NUMA systems
 *
 * PERFORMANCE CHARACTERISTICS:
 *    - Allocation: O(1) - single CAS operation
 *    - Deallocation: O(1) - single CAS operation
 *    - No memory allocator overhead after pool creation
 *    - Cache-friendly: sequential memory layout, minimal pointer chasing
 *
 * ============================================================================
 */

namespace pubsub_itc_fw {

/**
 * @class FixedSizeMemoryPool
 * @brief A lock-free, fixed-size memory pool for fast object allocation.
 *
 * This class provides a thread-safe memory pool that allocates fixed-size
 * blocks of memory without using locks. It uses hardware atomic operations
 * (CMPXCHG16B on x86-64) to manage a lock-free free list.
 *
 * The pool allocates all memory up-front using mmap() and never returns it
 * to the OS until destruction. This provides predictable performance and
 * avoids fragmentation.
 *
 * @tparam T The type of object to allocate. Must be at least sizeof(void*)
 *           bytes to accommodate the intrusive free list structure.
 *
 * @note This class returns RAW MEMORY only. The caller is responsible for
 *       object construction (via placement new) and destruction.
 *
 * @warning Requires compilation with -mcx16 flag on x86-64 platforms.
 *
 * Thread Safety: This class is fully thread-safe. Multiple threads can
 *                call allocate() and deallocate() concurrently without
 *                external synchronization.
 */
template <typename T>
class FixedSizeMemoryPool final {
public:
    /**
     * @brief Constructs a memory pool with the specified configuration.
     *
     * @param objects_per_pool Number of objects the pool can hold
     * @param use_huge_pages_flag Whether to attempt huge page allocation
     * @param handler_for_huge_pages_error Callback invoked if huge page allocation fails
     *
     * @throws std::bad_alloc if memory allocation fails
     *
     * @note The pool allocates all memory immediately during construction.
     *       If huge pages are requested but unavailable, the pool falls back
     *       to standard pages and invokes the error handler.
     */
    FixedSizeMemoryPool(int objects_per_pool,
                        UseHugePagesFlag use_huge_pages_flag,
                        std::function<void(void*, size_t)> handler_for_huge_pages_error);

    /**
     * @brief Destructor - unmaps the memory allocated during construction.
     */
    ~FixedSizeMemoryPool();

    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;

    /**
     * @brief Allocates raw memory for one object from the pool.
     *
     * This method is lock-free and thread-safe. It uses a compare-and-swap
     * operation to atomically pop a node from the free list.
     *
     * @return Pointer to uninitialized memory, or nullptr if pool is exhausted
     *
     * @note The returned memory is NOT initialized. Caller must use placement
     *       new to construct an object: new (ptr) T(args...)
     */
    T* allocate();

    /**
     * @brief Returns raw memory to the pool.
     *
     * This method is lock-free and thread-safe. It uses a compare-and-swap
     * operation to atomically push a node onto the free list.
     *
     * @param node_to_push Pointer to memory previously obtained from allocate()
     *
     * @note The caller must have already destructed any object at this address.
     *       This method does NOT call destructors.
     *
     * @note Passing nullptr is safe and does nothing.
     */
    void deallocate(T* node_to_push);

    /**
     * @brief Checks if a pointer belongs to this pool's memory region.
     *
     * @param ptr Pointer to check
     * @return true if ptr is within this pool's allocated memory range
     */
    [[nodiscard]] bool contains(const T* ptr) const;

    /**
     * @brief Checks if the pool has no available objects.
     *
     * @return true if all objects are currently allocated
     *
     * @note This is a snapshot in time. The pool may become non-full
     *       immediately after this call returns in a multithreaded environment.
     */
    [[nodiscard]] bool is_full() const;

    /**
     * @brief Counts the number of available (unallocated) objects.
     *
     * @return Number of objects currently in the free list
     *
     * @note This operation walks the free list and is O(N) where N is the
     *       number of free objects. Use sparingly in hot paths.
     *
     * @warning The returned value is a snapshot and may be stale immediately
     *          in a multithreaded environment.
     */
    [[nodiscard]] int get_number_of_available_objects() const;

    /**
     * @brief Checks if this pool successfully allocated memory using huge pages.
     *
     * @return true if the pool is using 2MB huge pages
     */
    [[nodiscard]] bool uses_huge_pages() const;

    /**
     * @brief Returns the huge page size if huge pages are in use.
     *
     * @return Huge page size in bytes (2MB), or 0 if not using huge pages
     */
    [[nodiscard]] size_t get_huge_page_size() const;

    /**
     * @brief Sets the next pool in a linked chain (for ExpandablePoolAllocator).
     *
     * @param next Pointer to the next pool, or nullptr if this is the tail
     *
     * @note Uses atomic store with RELEASE ordering to ensure visibility.
     */
    void set_next_pool(FixedSizeMemoryPool<T>* next) { 
        __atomic_store_n(&next_pool_, next, __ATOMIC_RELEASE);
    }
    
    /**
     * @brief Gets the next pool in a linked chain (for ExpandablePoolAllocator).
     *
     * @return Pointer to the next pool, or nullptr if this is the tail
     *
     * @note Uses atomic load with ACQUIRE ordering to ensure visibility.
     */
    FixedSizeMemoryPool<T>* get_next_pool() const { 
        return __atomic_load_n(&next_pool_, __ATOMIC_ACQUIRE);
    }

private:
    /**
     * @brief Node structure for the intrusive free list.
     *
     * When an object is in the free list, its memory is reused to store
     * the 'next' pointer, forming a singly-linked stack.
     */
    struct FreeListNode {
        FreeListNode* next;  ///< Pointer to next free node, or nullptr if tail
    };

    /**
     * @brief Tagged pointer structure for ABA prevention.
     *
     * Combines a pointer with a monotonically increasing counter to create
     * a unique 128-bit value for each state of the free list head.
     *
     * @note Must be 16-byte aligned for CMPXCHG16B instruction.
     */
    struct alignas(16) HeadPtr {
        FreeListNode* ptr;  ///< Pointer to the head of the free list
        uint64_t counter;   ///< ABA-prevention counter, incremented on every change
    };

    /**
     * @brief Pushes a node onto the free list (used during initialization and deallocation).
     *
     * @param node_to_push Pointer to the memory to return to the free list
     */
    void push_node_to_free_list(T* node_to_push);

    /**
     * @brief Pops a node from the free list (used during allocation).
     *
     * @return Pointer to allocated memory, or nullptr if list is empty
     */
    T* pop_node_from_free_list();

    /**
     * @brief Atomically loads the free list head with ACQUIRE semantics.
     *
     * @return Current value of the free list head (pointer + counter)
     *
     * @note Uses __atomic_load to ensure we see all writes that happened-before
     *       the head was stored.
     */
    inline HeadPtr load_head() const noexcept {
        unsigned __int128 val;
        __atomic_load(&head_raw_, &val, __ATOMIC_ACQUIRE);
        return *reinterpret_cast<const HeadPtr*>(&val);
    }

    /**
     * @brief Attempts to atomically update the free list head.
     *
     * This is the core of the lock-free algorithm. It uses CMPXCHG16B to
     * atomically compare-and-swap the 128-bit head value.
     *
     * @param expected Current expected value. Updated with actual value on failure.
     * @param desired New value to write if expected matches current value
     * @return true if swap succeeded, false if another thread modified the head
     *
     * @note Uses ACQ_REL ordering on success (our writes visible before update,
     *       we see all prior writes). Uses ACQUIRE on failure (we see actual value).
     *
     * @note The "weak" variant may spuriously fail even when expected==current.
     *       This is acceptable in a loop (as used in push/pop).
     */
    inline bool compare_exchange_weak(HeadPtr& expected, HeadPtr desired) noexcept {
        return __atomic_compare_exchange(
            &head_raw_,
            reinterpret_cast<unsigned __int128*>(&expected),
            reinterpret_cast<unsigned __int128*>(&desired),
            true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    int objects_per_pool_;                    ///< Total capacity of the pool
    UseHugePagesFlag use_huge_pages_flag_;    ///< Actual huge pages status (may differ from request)
    size_t total_pool_size_;                  ///< Total bytes allocated via mmap
    void* pool_memory_;                       ///< Base address of mmap'd memory

    /**
     * @brief Lock-free free list head (pointer + ABA counter).
     *
     * This is the heart of the lock-free algorithm. The 128-bit value contains:
     * - [0-63]: Pointer to the head of the free list
     * - [64-127]: Monotonic counter for ABA prevention
     *
     * @note Must be 16-byte aligned for CMPXCHG16B instruction.
     * @note Mutable because load_head() is const but must use __atomic_load.
     */
    alignas(16) mutable unsigned __int128 head_raw_{0};

    /**
     * @brief Pointer to the next pool in a chain (used by ExpandablePoolAllocator).
     *
     * This allows multiple pools to be linked together for expandable allocation.
     */
    FixedSizeMemoryPool<T>* next_pool_{nullptr};
};

template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool,
                                            UseHugePagesFlag use_huge_pages_flag,
                                            std::function<void(void*, size_t)> handler_for_huge_pages_error)
    : objects_per_pool_(objects_per_pool),
      use_huge_pages_flag_(use_huge_pages_flag) {
    
    // Compile-time verification that hardware supports 128-bit CAS
#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
    static_assert(false, "Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags.");
#endif
    static_assert(alignof(HeadPtr) == 16, "HeadPtr must be 16-byte aligned.");

    // Calculate memory requirements (objects must be large enough for intrusive linking)
    size_t object_size = std::max(sizeof(T), sizeof(FreeListNode));
    total_pool_size_ = static_cast<size_t>(objects_per_pool_) * object_size;

    // Attempt allocation with requested page type
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
        flags |= MAP_HUGETLB;
    }

    pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);

    // Fallback to standard pages if huge pages requested but unavailable
    if (pool_memory_ == MAP_FAILED && (flags & MAP_HUGETLB)) {
        if (handler_for_huge_pages_error) {
            handler_for_huge_pages_error(nullptr, total_pool_size_);
        }
        
        use_huge_pages_flag_ = UseHugePagesFlag::DoNotUseHugePages;
        flags &= ~MAP_HUGETLB;
        pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
    }

    if (pool_memory_ == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Initialize the free list by pushing all objects onto the stack
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
bool FixedSizeMemoryPool<T>::uses_huge_pages() const { 
    return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages; 
}

template <typename T>
size_t FixedSizeMemoryPool<T>::get_huge_page_size() const { 
    return uses_huge_pages() ? 2048 * 1024 : 0; 
}

template <typename T>
void FixedSizeMemoryPool<T>::push_node_to_free_list(T* node_to_push) {
    FreeListNode* new_node = reinterpret_cast<FreeListNode*>(node_to_push);
    HeadPtr old_head = load_head();
    HeadPtr next_head;
    
    // Lock-free push: retry until CAS succeeds
    do {
        new_node->next = old_head.ptr;           // Link new node to current head
        next_head.ptr = new_node;                // New head points to our node
        next_head.counter = old_head.counter + 1; // Increment ABA counter
    } while (!compare_exchange_weak(old_head, next_head));
}

template <typename T>
T* FixedSizeMemoryPool<T>::pop_node_from_free_list() {
    HeadPtr old_head = load_head();
    HeadPtr next_head;
    
    // Lock-free pop: retry until CAS succeeds or list is empty
    while (old_head.ptr != nullptr) {
        next_head.ptr = old_head.ptr->next;       // New head is next in list
        next_head.counter = old_head.counter + 1;  // Increment ABA counter
        
        if (compare_exchange_weak(old_head, next_head)) {
            return reinterpret_cast<T*>(old_head.ptr);
        }
        // CAS failed - another thread modified the list. old_head now contains
        // the current value, so we loop and try again.
    }
    return nullptr; // List is empty
}

} // namespace pubsub_itc_fw

#pragma GCC diagnostic pop
