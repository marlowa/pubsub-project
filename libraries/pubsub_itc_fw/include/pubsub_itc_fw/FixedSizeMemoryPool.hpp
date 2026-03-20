#pragma once

/**
 * @warning This class requires the compiler flag @c -mcx16 on x86-64 platforms.
 *          This flag enables the @c CMPXCHG16B instruction, which provides
 *          hardware 128-bit compare-and-swap used by the lock-free free list.
 *          Without it, the @c static_assert in this constructor will fire at
 *          compile time with the message:
 *          "Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags."
 *          In CMake, add the following to your target:
 *          @code
 *          target_compile_options(your_target PRIVATE -mcx16)
 *          @endcode
 *          This flag is safe on all x86-64 processors manufactured after
 *          approximately 2006. It is not required when building with
 *          @c USING_VALGRIND defined, as that build path uses a mutex-based
 *          implementation instead.
 */

#ifdef USING_VALGRIND

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

namespace pubsub_itc_fw {

template <typename T> struct Slot;

template <typename T> struct SlotStorage {
    struct FreeListNode {
        Slot<T>* next;
    };

    union {
        FreeListNode free_node;
        std::aligned_storage_t<sizeof(T), alignof(T)> storage;
    };

    T* object_ptr() {
        return reinterpret_cast<T*>(&storage);
    }
    T const* object_ptr() const {
        return reinterpret_cast<T const*>(&storage);
    }
    FreeListNode* free_node_ptr() {
        return &free_node;
    }
};

template <typename T> struct Slot {
    std::uintptr_t flag; // 0 = free, 1 = allocated
    SlotStorage<T> storage;
};

/**
 * @brief Valgrind-friendly FixedSizeMemoryPool.
 * * Provides mutex-protected memory management that satisfies Valgrind's
 * synchronization requirements while maintaining compatibility with the
 * ExpandablePoolAllocator's flag-based diagnostic checks.
 */
template <typename T> class FixedSizeMemoryPool {
  public:
    using SlotType = Slot<T>;

    FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag, std::function<void(void*, std::size_t)> handler_for_huge_pages_error)
        : objects_per_pool_(objects_per_pool), use_huge_pages_flag_(use_huge_pages_flag) {
        total_pool_size_ = static_cast<std::size_t>(objects_per_pool_) * sizeof(SlotType);

        pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (pool_memory_ == MAP_FAILED)
            throw std::bad_alloc();

        slots_ = reinterpret_cast<SlotType*>(pool_memory_);
        free_list_v_.reserve(objects_per_pool_);

        for (int i = 0; i < objects_per_pool_; ++i) {
            slots_[i].flag = 0;
            free_list_v_.push_back(&slots_[i]);
        }
    }

    /**
     * @brief Pool Destructor.
     * * Only calls destructors on objects that were never returned to the pool
     * (leaks). This ensures consistency with the ExpandablePoolAllocator's
     * ownership model.
     */
    ~FixedSizeMemoryPool() {
        if (pool_memory_ != MAP_FAILED) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < objects_per_pool_; ++i) {
                if (slots_[i].flag == 1) {
                    slots_[i].storage.object_ptr()->~T();
                }
            }
            munmap(pool_memory_, total_pool_size_);
        }
    }

    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;

    [[nodiscard]] T* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_v_.empty())
            return nullptr;

        SlotType* slot = free_list_v_.back();
        free_list_v_.pop_back();

        slot->flag = 1;
        allocation_count_++;
        return slot->storage.object_ptr();
    }

    /**
     * @brief Returns raw memory to the pool.
     * * Does NOT call the destructor, as ExpandablePoolAllocator has already
     * handled the object lifetime before calling this method.
     */
    void deallocate(T* ptr) {
        if (!ptr)
            throw PreconditionAssertion("deallocate called with nullptr", __FILE__, __LINE__);

        SlotType* slot = reinterpret_cast<SlotType*>(reinterpret_cast<char*>(ptr) - offsetof(SlotType, storage.storage));

        std::lock_guard<std::mutex> lock(mutex_);
        slot->flag = 0;
        free_list_v_.push_back(slot);
    }

    [[nodiscard]] bool contains(T const* ptr) const {
        auto const* byte_ptr = reinterpret_cast<std::byte const*>(ptr);
        auto const* start = static_cast<std::byte const*>(pool_memory_);
        return (byte_ptr >= start && byte_ptr < (start + total_pool_size_));
    }

    [[nodiscard]] bool is_full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_v_.empty();
    }

    [[nodiscard]] int get_number_of_available_objects() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(free_list_v_.size());
    }

    [[nodiscard]] bool uses_huge_pages() const {
        return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages;
    }

    [[nodiscard]] std::size_t get_huge_page_size() const {
        return uses_huge_pages() ? 2048U * 1024U : 0U;
    }

    uint64_t get_allocation_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocation_count_;
    }

    void set_next_pool(FixedSizeMemoryPool<T>* next) {
        __atomic_store_n(&next_pool_, next, __ATOMIC_RELEASE);
    }

    [[nodiscard]] FixedSizeMemoryPool<T>* get_next_pool() const {
        return __atomic_load_n(&next_pool_, __ATOMIC_ACQUIRE);
    }

  private:
    int objects_per_pool_;
    UseHugePagesFlag use_huge_pages_flag_;
    std::size_t total_pool_size_{0};
    void* pool_memory_{MAP_FAILED};
    SlotType* slots_{nullptr};

    mutable std::mutex mutex_;
    std::vector<SlotType*> free_list_v_;
    uint64_t allocation_count_{0};
    FixedSizeMemoryPool<T>* next_pool_{nullptr};
};

} // namespace pubsub_itc_fw
#else

// This diagnostic suppression is required because unsigned __int128 is a GNU extension,
// and we rely on it for the 128-bit tagged pointer used in the Treiber stack.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <stdexcept>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

/*
 * ============================================================================
 * LOCK-FREE MEMORY POOL DESIGN OVERVIEW
 * ============================================================================
 *
 * This memory pool implements a lock-free stack (Treiber stack) for object
 * allocation using an ABA-prevention technique with a tagged pointer.
 *
 * KEY DESIGN ELEMENTS:
 *
 * 1. INTRUSIVE FREE LIST VIA SLOT STORAGE
 *    - Each pool element is a Slot<T>, which contains:
 *        * a flag indicating whether the slot is allocated or free
 *        * a SlotStorage<T> union that can hold either:
 *            - a FreeListNode (used when the slot is on the free list), or
 *            - raw storage for a T object (used when the slot is allocated)
 *    - The free list is intrusive: when a slot is free, its own memory stores
 *      the 'next' pointer in the FreeListNode.
 *    - No separate node allocations are required.
 *
 * 2. ABA PROBLEM PREVENTION
 *    - Classic ABA scenario: Thread A reads head->X, gets pre-empted, Thread B pops X
 *      and Y, then pushes X back. Thread A resumes and CAS succeeds incorrectly.
 *    - Solution: use a 128-bit structure with {pointer, counter}.
 *    - Every CAS operation increments the counter, making each pointer+counter pair unique.
 *    - Even if the same address is reused, the counter will differ, preventing ABA.
 *
 * 3. 128-BIT ATOMIC OPERATIONS
 *    - On x86-64, CMPXCHG16B is used for 128-bit compare-and-swap.
 *    - Requirements:
 *        * -mcx16 compiler flag (see constructor @warning for CMake instructions)
 *        * 16-byte alignment of the head structure (enforced by alignas(16))
 *        * __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16 defined at compile time
 *    - unsigned __int128 is used as the underlying storage type.
 *    - GCC built-in __atomic_compare_exchange compiles directly to CMPXCHG16B.
 *    - No libatomic dependency is required; these are true compiler intrinsics.
 *
 * 4. WHY NOT std::atomic<HeadPtr>?
 *    - std::atomic<struct> with runtime is_lock_free() checks can link against libatomic.
 *    - libatomic may conservatively report 'false' even when hardware supports the operation.
 *    - Using unsigned __int128 with __atomic_* intrinsics bypasses libatomic entirely.
 *    - GCC recognises unsigned __int128 and emits CMPXCHG16B directly.
 *
 * 5. MEMORY ORDERING
 *    - load_head(): ACQUIRE ordering – ensures we see all prior writes to the node.
 *    - compare_exchange_weak(): ACQ_REL on success, ACQUIRE on failure:
 *        * RELEASE: writes to the new head are visible before the head update.
 *        * ACQUIRE: we see the actual head value if CAS fails.
 *
 * 6. MEMORY ALLOCATION STRATEGY
 *    - Uses mmap() for predictable, contiguous allocation.
 *    - Optional huge pages (2MB) for reduced TLB pressure on NUMA systems.
 *    - Falls back to standard pages if huge pages are unavailable.
 *    - Memory is never freed until pool destruction (no munmap during operation).
 *
 * 7. THREAD SAFETY GUARANTEES
 *    - Multiple threads can allocate() and deallocate() concurrently.
 *    - No locks, no blocking – lock-free Treiber stack for the free list.
 *    - Safe under x86-64 TSO (Total Store Order).
 *
 * 8. SAFETY OF TREIBER STACK IN NON-GC ENVIRONMENTS
 *    - Traditional Treiber stacks in non-garbage-collected languages require
 *      careful handling of memory reclamation, because nodes may be freed and
 *      later reused for unrelated objects. This can reintroduce the ABA problem
 *      even when a counter is used.
 *
 *    - This allocator does not free individual nodes. All nodes are allocated
 *      up-front during pool construction and remain valid for the entire
 *      lifetime of the pool. The only OS-level free operation occurs when the
 *      entire pool is unmapped during destruction.
 *
 *    - Nodes are reused only within the same pool. A node's address may appear
 *      again in the free list after deallocation, but always as the same
 *      logical node, never as memory that has been returned to the operating
 *      system or repurposed for a different object.
 *
 *    - Because nodes are never individually reclaimed, and because each update
 *      to the free-list head increments a monotonically increasing counter, the
 *      ABA-prevention scheme is sufficient and safe. The counter ensures that
 *      even if a pointer value repeats, the tagged pointer does not.
 *
 *    - This design avoids the memory-reclamation hazards normally associated
 *      with Treiber stacks in manual-memory-management environments.
 *
 * ============================================================================
 */

namespace pubsub_itc_fw {

template <typename T> struct SlotStorage; // forward declaration

// ============================================================
// Slot<T> — one allocator slot: [flag][storage]
// ============================================================

template <typename T> struct Slot {
    std::uintptr_t flag; // 0 = free, 1 = allocated
    SlotStorage<T> storage;
};

// ============================================================
// SlotStorage<T> — raw storage helper for allocator slots
// ============================================================

template <typename T> struct SlotStorage {
    struct FreeListNode {
        Slot<T>* next;
    };

    union {
        FreeListNode free_node;
        std::aligned_storage_t<sizeof(T), alignof(T)> storage;
    };

    T* object_ptr() {
        return reinterpret_cast<T*>(&storage);
    }

    T const* object_ptr() const {
        return reinterpret_cast<T const*>(&storage);
    }

    FreeListNode* free_node_ptr() {
        return &free_node;
    }

    FreeListNode const* free_node_ptr() const {
        return &free_node;
    }
};

// ============================================================
// FixedSizeMemoryPool<T>
// ============================================================

/**
 * @brief A lock-free, fixed-size memory pool for fast object allocation.
 *
 * This class provides a thread-safe memory pool that allocates fixed-size
 * slots of memory without using locks. It uses hardware atomic operations
 * (CMPXCHG16B on x86-64) to manage a lock-free free list of Slot<T> nodes.
 *
 * The pool allocates all memory up-front using mmap() and never returns it
 * to the operating system until destruction. This provides predictable
 * performance and avoids fragmentation.
 *
 * @tparam T The type of object to allocate.
 *
 * @note This class returns raw memory only. The caller is responsible for
 *       object construction (via placement new) and destruction.
 *
 * @warning Requires compilation with -mcx16 flag on x86-64 platforms.
 *
 * Thread safety: This class is fully thread-safe. Multiple threads can
 *                call allocate() and deallocate() concurrently without
 *                external synchronisation.
 */
template <typename T> class FixedSizeMemoryPool {
  public:
    using SlotType = Slot<T>;

    struct alignas(16) HeadPtr {
        SlotType* ptr;
        std::uint64_t counter;
    };

    /**
     * @brief Destructor – unmaps the memory allocated during construction.
     */
    ~FixedSizeMemoryPool();

    /**
     * @brief Constructs a memory pool with the specified configuration.
     *
     * @param[in] objects_per_pool Number of objects the pool can hold.
     * @param[in] use_huge_pages_flag Whether to attempt huge page allocation.
     * @param[in] handler_for_huge_pages_error Callback invoked if huge page allocation fails.
     *
     * @throws std::bad_alloc if memory allocation fails.
     *
     * @note The pool allocates all memory immediately during construction.
     *       If huge pages are requested but unavailable, the pool falls back
     *       to standard pages and invokes the error handler.
     */
    FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag, std::function<void(void*, std::size_t)> handler_for_huge_pages_error);

    FixedSizeMemoryPool(FixedSizeMemoryPool const&) = delete;
    FixedSizeMemoryPool& operator=(FixedSizeMemoryPool const&) = delete;
    FixedSizeMemoryPool(FixedSizeMemoryPool&&) = delete;
    FixedSizeMemoryPool& operator=(FixedSizeMemoryPool&&) = delete;

    /**
     * @brief Allocates raw memory for one object from the pool.
     *
     * This function returns a pointer to uninitialised storage for a T object.
     * The caller is responsible for constructing the object via placement new.
     *
     * This method is lock-free and thread-safe. It uses a compare-and-swap
     * operation to atomically pop a slot from the free list.
     *
     * @return T* Pointer to storage for a T object, or nullptr if this pool
     *         has no free slots remaining.
     *
     * @note A nullptr return value indicates that the pool is exhausted.
     *       It is the caller's responsibility (typically ExpandablePoolAllocator)
     *       to allocate a new FixedSizeMemoryPool and chain it to the pool list.
     *       The returned memory is not initialised. The caller must use placement
     *       new to construct an object: new (ptr) T(args...).
     */
    [[nodiscard]] T* allocate();

    /**
     * @brief Returns raw memory to the pool.
     *
     * This method is lock-free and thread-safe. It uses a compare-and-swap
     * operation to atomically push a slot onto the free list.
     *
     * @param[in] node_to_push Pointer to memory previously obtained from allocate().
     *
     * @note The caller must have already destructed any object at this address.
     *       This method does not call destructors.
     *
     * @throws PreconditionAssertion if node_to_push is nullptr.
     */
    void deallocate(T* node_to_push);

    /**
     * @brief Checks if a pointer belongs to this pool's memory region.
     *
     * @param[in] ptr Pointer to check.
     * @return true if ptr is within this pool's allocated memory range.
     */
    [[nodiscard]] bool contains(T const* ptr) const;

    /**
     * @brief Checks if the pool has no available objects.
     *
     * @return true if all objects are currently allocated.
     *
     * @note This is a snapshot in time. The pool may become non-full
     *       immediately after this call returns in a multithreaded environment.
     */
    [[nodiscard]] bool is_full() const;

    /**
     * @brief Counts the number of available (unallocated) objects.
     *
     * @return Number of objects currently in the free list.
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
     * @return true if the pool is using 2MB huge pages.
     */
    [[nodiscard]] bool uses_huge_pages() const;

    /**
     * @brief Returns the huge page size if huge pages are in use.
     *
     * @return Huge page size in bytes (2MB), or 0 if not using huge pages.
     */
    [[nodiscard]] std::size_t get_huge_page_size() const;

    /**
     * @brief Sets the next pool in a linked chain (for ExpandablePoolAllocator).
     *
     * @param[in] next Pointer to the next pool, or nullptr if this is the tail.
     *
     * @note Uses atomic store with RELEASE ordering to ensure visibility.
     */
    void set_next_pool(FixedSizeMemoryPool<T>* next) {
        __atomic_store_n(&next_pool_, next, __ATOMIC_RELEASE);
    }

    /**
     * @brief Gets the next pool in a linked chain (for ExpandablePoolAllocator).
     *
     * @return Pointer to the next pool, or nullptr if this is the tail.
     *
     * @note Uses atomic load with ACQUIRE ordering to ensure visibility.
     */
    [[nodiscard]] FixedSizeMemoryPool<T>* get_next_pool() const {
        return __atomic_load_n(&next_pool_, __ATOMIC_ACQUIRE);
    }

    // TODO not safe make it an atomic
    uint64_t get_allocation_count() const {
        return allocation_count_.load();
    }

  private:
    /**
     * @brief Converts a slot pointer to the corresponding object pointer.
     *
     * @param[in] slot Pointer to a SlotType.
     * @return T* Pointer to the object storage within the slot.
     */
    static T* object_from_slot(SlotType* slot) {
        return slot->storage.object_ptr();
    }

    /**
     * @brief Converts an object pointer to the corresponding slot pointer.
     *
     * @param[in] obj Pointer to an object within this pool.
     * @return SlotType* Pointer to the owning slot.
     */
    static SlotType* slot_from_object(T* obj) {
        auto* storage_ptr = reinterpret_cast<std::aligned_storage_t<sizeof(T), alignof(T)>*>(obj);

        auto* slot = reinterpret_cast<SlotType*>(reinterpret_cast<char*>(storage_ptr) - offsetof(SlotType, storage));

        return slot;
    }

    /**
     * @brief Atomically loads the free list head with ACQUIRE semantics.
     *
     * @return Current value of the free list head (pointer + counter).
     *
     * @note Uses __atomic_load to ensure we see all writes that happened-before
     *       the head was stored.
     */
    [[nodiscard]] HeadPtr load_head() const {
        unsigned __int128 val;
        __atomic_load(&head_raw_, &val, __ATOMIC_ACQUIRE);
        return *reinterpret_cast<HeadPtr const*>(&val);
    }

    /**
     * @brief Attempts to atomically update the free list head.
     *
     * This is the core of the lock-free algorithm. It uses CMPXCHG16B to
     * atomically compare-and-swap the 128-bit head value.
     *
     * @param[in,out] expected Current expected value. Updated with actual value on failure.
     * @param[in] desired New value to write if expected matches current value.
     * @return true if swap succeeded, false if another thread modified the head.
     *
     * @note Uses ACQ_REL ordering on success (our writes visible before update,
     *       we see all prior writes). Uses ACQUIRE on failure (we see actual value).
     *
     * @note The "weak" variant may spuriously fail even when expected == current.
     *       This is acceptable in a loop (as used in push/pop).
     */
    bool compare_exchange_weak(HeadPtr& expected, HeadPtr desired) {
        return __atomic_compare_exchange(&head_raw_, reinterpret_cast<unsigned __int128*>(&expected), reinterpret_cast<unsigned __int128*>(&desired), true,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    /**
     * @brief Pushes a slot onto the intrusive free list.
     *
     * @param[in] slot Pointer to the slot to push.
     *
     * @note This function is lock-free and may be called concurrently.
     */
    void push_slot_to_free_list(SlotType* slot);

    /**
     * @brief Pops a slot from the intrusive free list.
     *
     * @return SlotType* Pointer to the popped slot, or nullptr if the list is empty.
     *
     * @note This function is lock-free and may be called concurrently.
     */
    [[nodiscard]] SlotType* pop_slot_from_free_list();

    int objects_per_pool_;
    UseHugePagesFlag use_huge_pages_flag_;
    std::size_t total_pool_size_{0U};
    void* pool_memory_{MAP_FAILED};
    SlotType* slots_{nullptr};

    alignas(16) mutable unsigned __int128 head_raw_{0U};

    FixedSizeMemoryPool<T>* next_pool_{nullptr};
    std::atomic<uint64_t> allocation_count_{0};
};

template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag,
                                            std::function<void(void*, std::size_t)> handler_for_huge_pages_error)
    : objects_per_pool_(objects_per_pool), use_huge_pages_flag_(use_huge_pages_flag) {
#ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16
    static_assert(false, "Hardware 128-bit atomics not supported. Add -mcx16 to compiler flags.");
#endif
    static_assert(alignof(HeadPtr) == 16, "HeadPtr must be 16-byte aligned.");

    // Calculate memory requirements: pool stores an array of SlotType.
    total_pool_size_ = static_cast<std::size_t>(objects_per_pool_) * sizeof(SlotType);

    // Attempt allocation with requested page type.
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
        flags |= MAP_HUGETLB;
    }

    pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);

    // Fallback to standard pages if huge pages requested but unavailable.
    if (pool_memory_ == MAP_FAILED && (flags & MAP_HUGETLB) != 0) {
        if (handler_for_huge_pages_error) {
            handler_for_huge_pages_error(nullptr, total_pool_size_);
        }

        use_huge_pages_flag_ = UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages);
        flags &= ~MAP_HUGETLB;
        pool_memory_ = mmap(nullptr, total_pool_size_, PROT_READ | PROT_WRITE, flags, -1, 0);
    }

    if (pool_memory_ == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Interpret the mmap region as an array of SlotType and initialise the free list.
    slots_ = reinterpret_cast<SlotType*>(pool_memory_);

    for (int i = 0; i < objects_per_pool_; ++i) {
        SlotType* slot = &slots_[i];
        slot->flag = 0U;
        slot->storage.free_node_ptr()->next = nullptr;
        push_slot_to_free_list(slot);
    }
}

template <typename T> FixedSizeMemoryPool<T>::~FixedSizeMemoryPool() {

    if (pool_memory_ != MAP_FAILED) {
        // Destruct any objects that are still allocated in this pool.
        for (int i = 0; i < objects_per_pool_; ++i) {
            SlotType* slot = &slots_[i];
            if (slot->flag == 1U) {
                T* obj = slot->storage.object_ptr();
                obj->~T();
                slot->flag = 0U;
            }
        }

        munmap(pool_memory_, total_pool_size_);
    }
}

template <typename T> T* FixedSizeMemoryPool<T>::allocate() {
    SlotType* slot = pop_slot_from_free_list();
    if (slot == nullptr) {
        return nullptr;
    }
    allocation_count_.fetch_add(1, std::memory_order_relaxed);;
    return object_from_slot(slot);
}

template <typename T> void FixedSizeMemoryPool<T>::deallocate(T* node_to_push) {
    if (node_to_push == nullptr) {
        throw PreconditionAssertion("deallocate called with nullptr", __FILE__, __LINE__);
    }

    SlotType* slot = slot_from_object(node_to_push);
    push_slot_to_free_list(slot);
}

template <typename T> bool FixedSizeMemoryPool<T>::contains(T const* ptr) const {
    auto const* byte_ptr = reinterpret_cast<std::byte const*>(ptr);
    auto const* start = static_cast<std::byte const*>(pool_memory_);
    auto const* end = start + total_pool_size_;
    return (byte_ptr >= start && byte_ptr < end);
}

template <typename T> bool FixedSizeMemoryPool<T>::is_full() const {
    return load_head().ptr == nullptr;
}

template <typename T> int FixedSizeMemoryPool<T>::get_number_of_available_objects() const {
    int count = 0;
    HeadPtr head = load_head();
    SlotType* current = head.ptr;
    while (current != nullptr) {
        ++count;
        current = current->storage.free_node_ptr()->next;
    }
    return count;
}

template <typename T> bool FixedSizeMemoryPool<T>::uses_huge_pages() const {
    return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages;
}

template <typename T> std::size_t FixedSizeMemoryPool<T>::get_huge_page_size() const {
    if (uses_huge_pages()) {
        return static_cast<std::size_t>(2048U) * 1024U;
    }
    return 0U;
}

template <typename T> void FixedSizeMemoryPool<T>::push_slot_to_free_list(SlotType* slot) {
    HeadPtr old_head = load_head();
    HeadPtr next_head;

    do {
        slot->storage.free_node_ptr()->next = old_head.ptr;
        next_head.ptr = slot;
        next_head.counter = old_head.counter + 1U;
    } while (!compare_exchange_weak(old_head, next_head));
}

template <typename T> typename FixedSizeMemoryPool<T>::SlotType* FixedSizeMemoryPool<T>::pop_slot_from_free_list() {
    HeadPtr old_head = load_head();
    HeadPtr next_head;

    while (old_head.ptr != nullptr) {
        SlotType* head_slot = old_head.ptr;
        next_head.ptr = head_slot->storage.free_node_ptr()->next;
        next_head.counter = old_head.counter + 1U;

        if (compare_exchange_weak(old_head, next_head)) {
            return head_slot;
        }
    }

    return nullptr;
}

} // namespace pubsub_itc_fw

#pragma GCC diagnostic pop

#endif
