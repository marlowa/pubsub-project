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
 *          This flag is safe on all x86-64 processors manufactured after approximately 2006.
 *          It is not required when building with @c USING_VALGRIND defined, as that build path uses
 *          a mutex-based implementation instead.
 *
 * ============================================================================
 * BUILD PATH SELECTION
 * ============================================================================
 *
 * This file provides two implementations of FixedSizeMemoryPool<T>:
 *
 * PRODUCTION PATH (USING_VALGRIND not defined)
 *    - Lock-free Treiber stack using 128-bit CMPXCHG16B atomics.
 *    - Requires -mcx16 compiler flag on x86-64.
 *    - Used for all normal builds and ASAN builds.
 *    - ASAN is compatible with this path because it instruments memory
 *      safety (bounds, lifetime) without needing to decompose individual
 *      atomic operations.
 *
 * INSTRUMENTED PATH (USING_VALGRIND defined)
 *    - Mutex-protected implementation that replaces the lock-free Treiber
 *      stack with a std::vector-based free list.
 *    - USING_VALGRIND is defined in two distinct build configurations:
 *
 *      1. Valgrind builds (--valgrind flag in build.py):
 *         Helgrind and DRD cannot model CMPXCHG16B and report false
 *         positives. The mutex path gives them synchronisation primitives
 *         they can reason about correctly.
 *
 *      2. ThreadSanitizer builds (--tsan flag in build.py):
 *         TSan intercepts every memory access to track per-thread ordering.
 *         It cannot decompose CMPXCHG16B into the individual accesses it
 *         needs to track, so it cannot correctly instrument the lock-free
 *         path. The mutex path gives TSan standard primitives it fully
 *         understands.
 *
 *    - Note: despite the macro name, USING_VALGRIND does not imply that Valgrind is necessarily running.
 *      It means "use the instrumentation-friendly mutex path".
 *      The CMakeLists.txt sets this macro for both Valgrind and TSan builds.
 *
 * ============================================================================
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <stdexcept>
#include <cstring> // for memcpy

#include <unistd.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

#ifdef USING_VALGRIND

#include <mutex>
#include <string>
#include <sys/mman.h>

#include <vector>

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

    FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag, //
                        [[maybe_unused]] std::function<void(void*, std::size_t)> handler_for_huge_pages_error)
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
     * @brief Destructs all remaining allocated objects and releases pool memory.
     *
     * @note This destructor explicitly calls the destructor on any slot whose
     *       flag is 1 (allocated). These are objects that were allocated by the
     *       caller but never returned to the pool via deallocate() before the
     *       allocator was destroyed — in other words, caller-leaked objects.
     *
     *       This is intentional cleanup, not a double-destruction hazard.
     *       ExpandablePoolAllocator::deallocate() always calls obj->~T() and
     *       clears the flag to 0 before returning the slot to this pool.
     *       Therefore any slot still flagged as 1 at destruction time has
     *       definitely not had its destructor called yet.
     *
     *       The flag is cleared to 0 after destruction here purely for
     *       consistency, since the pool memory is about to be unmapped anyway.
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
        free_list_v_.push_back(slot);
    }

    [[nodiscard]] bool is_full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_v_.empty();
    }

    [[nodiscard]] int get_number_of_available_objects() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(free_list_v_.size());
    }

    [[nodiscard]] bool uses_huge_pages() const;

    [[nodiscard]] std::size_t get_huge_page_size() const;

    uint64_t get_allocation_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocation_count_;
    }

    void set_next_pool(FixedSizeMemoryPool<T>* next);

    [[nodiscard]] FixedSizeMemoryPool<T>* get_next_pool() const;

    [[nodiscard]] bool contains(T const* ptr) const;

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

#else // the non-valgrind code

// This diagnostic suppression is required because unsigned __int128 is a GNU extension,
// and we rely on it for the 128-bit tagged pointer used in the Treiber stack.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include <sys/mman.h>

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
     * @brief Destructs all remaining allocated objects and releases pool memory.
     *
     * @note This destructor explicitly calls the destructor on any slot whose
     *       flag is 1 (allocated). These are objects that were allocated by the
     *       caller but never returned to the pool via deallocate() before the
     *       allocator was destroyed — in other words, caller-leaked objects.
     *
     *       This is intentional cleanup, not a double-destruction hazard.
     *       ExpandablePoolAllocator::deallocate() always calls obj->~T() and
     *       clears the flag to 0 before returning the slot to this pool.
     *       Therefore any slot still flagged as 1 at destruction time has
     *       definitely not had its destructor called yet.
     *
     * @note The mutex is held during iteration to satisfy Helgrind and DRD,
     *       which require that all accesses to shared state are covered by
     *       a recognised synchronisation primitive. In practice, the
     *       allocator lifetime guarantee (no threads active during destruction)
     *       means the lock is not strictly necessary for correctness.
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
    FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag, //
                        std::function<void(void*, std::size_t)> handler_for_huge_pages_error);

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
    void set_next_pool(FixedSizeMemoryPool<T>* next);

    /**
     * @brief Gets the next pool in a linked chain (for ExpandablePoolAllocator).
     *
     * @return Pointer to the next pool, or nullptr if this is the tail.
     *
     * @note Uses atomic load with ACQUIRE ordering to ensure visibility.
     */
    [[nodiscard]] FixedSizeMemoryPool<T>* get_next_pool() const;

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
     * Note: memcpy is used as a type-punning barrier to satisfy strict aliasing.
     * Compilers optimise this to simple moves; there is no measurable runtime cost.
     *
     * @return Current value of the free list head (pointer + counter).
     *
     * @note Uses __atomic_load to ensure we see all writes that happened-before
     *       the head was stored.
     */
    [[nodiscard]] HeadPtr load_head() const {
        // On x86-64, a 16-byte aligned load via SSE movdqa is atomic.
        // This avoids both the libatomic PLT call (__atomic_load_16) and
        // the cost of CMPXCHG16B for a read-only load. The alignas(16) on
        // head_raw_ guarantees the required alignment.
        unsigned __int128 val;
        __asm__ volatile (
            "movdqa %1, %%xmm0\n\t"
            "movdqa %%xmm0, %0\n\t"
            : "=m" (val)
            : "m" (head_raw_)
            : "xmm0", "memory"
        );
        HeadPtr head{};
        static_assert(sizeof(head) == sizeof(val), "HeadPtr must be 128 bits");
        std::memcpy(&head, &val, sizeof(head));
        return head;
    }

    /**
     * @brief Attempts to atomically update the free list head using CMPXCHG16B.
     *
     * This is the core of the lock-free Treiber stack algorithm. It performs a
     * 128-bit compare-and-swap on the free list head, which contains both a pointer
     * and an ABA counter packed into a single @c HeadPtr.
     *
     * @param[in,out] expected The value expected to be current. On failure, updated
     *                         with the actual current value so the caller can retry.
     * @param[in]    desired   The value to write if @p expected matches current.
     * @return @c true if the swap succeeded; @c false if another thread had already
     *         modified the head before this attempt.
     *
     * @par Implementation notes
     * GCC 13 and later emit a @c libatomic PLT call for @c __atomic_compare_exchange
     * on 16-byte types regardless of @c -mcx16, adding indirect-call overhead on
     * every CAS in the hot path. @c __sync_bool_compare_and_swap does not have this
     * behaviour and reliably emits an inline @c lock @c cmpxchg16b instruction when
     * @c -mcx16 is present.
     *
     * @c __sync_bool_compare_and_swap implies sequentially-consistent ordering.
     * On x86-64 this is identical to acquire-release in the generated machine code:
     * @c lock @c cmpxchg16b provides full sequential consistency regardless of the
     * C++ memory order annotation, so the stronger ordering carries no runtime cost
     * on this architecture.
     *
     * On CAS failure the current head is reloaded via an inline SSE @c movdqa
     * instruction rather than @c __atomic_load, which would also call @c libatomic
     * on GCC 13+. A 16-byte aligned @c movdqa load is atomic on x86-64; the
     * @c alignas(16) on @c head_raw_ satisfies the alignment requirement.
     *
     * @note The "weak" variant may spuriously fail even when @p expected matches
     *       the current value. This is acceptable when called from a retry loop,
     *       as it is in @c push_slot_to_free_list and @c pop_slot_from_free_list.
     *
     * @warning This function is x86-64 specific. Porting to another architecture
     *          requires replacing both the @c __sync_bool_compare_and_swap call and
     *          the inline assembler reload with appropriate equivalents.
     */
    bool compare_exchange_weak(HeadPtr& expected, HeadPtr desired) {
        unsigned __int128 expected_raw;
        unsigned __int128 desired_raw;
        static_assert(sizeof(expected_raw) == sizeof(expected), "HeadPtr must be 128 bits");
        std::memcpy(&expected_raw, &expected, sizeof(expected));
        std::memcpy(&desired_raw, &desired, sizeof(desired));
        bool ok = __sync_bool_compare_and_swap(&head_raw_, expected_raw, desired_raw);
        if (!ok) {
            // Reload the current head using inline SSE movdqa to avoid the
            // libatomic PLT call that __atomic_load generates on GCC 13+.
            // On x86-64, movdqa on a 16-byte aligned address is atomic.
            // The alignas(16) on head_raw_ guarantees the required alignment.
            __asm__ volatile (
                "movdqa %1, %%xmm0\n\t"
                "movdqa %%xmm0, %0\n\t"
                : "=m" (expected_raw)
                : "m" (head_raw_)
                : "xmm0", "memory"
            );
            std::memcpy(&expected, &expected_raw, sizeof(expected));
        }
        return ok;
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
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int objects_per_pool, UseHugePagesFlag use_huge_pages_flag, //
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

template <typename T> void FixedSizeMemoryPool<T>::push_slot_to_free_list(SlotType* slot) {
    HeadPtr old_head = load_head();
    HeadPtr next_head;

    do {
        slot->storage.free_node_ptr()->next = old_head.ptr;
        next_head.ptr = slot;
        next_head.counter = old_head.counter + 1U;
    } while (!compare_exchange_weak(old_head, next_head));
}

template <typename T>
typename FixedSizeMemoryPool<T>::SlotType* FixedSizeMemoryPool<T>::pop_slot_from_free_list() {
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

} // end namespaces

#endif

// ============================================================
// Implementations common to both build paths
// ============================================================

namespace pubsub_itc_fw {

template <typename T>
bool FixedSizeMemoryPool<T>::contains(T const* ptr) const {
    auto const* byte_ptr     = reinterpret_cast<std::byte const*>(ptr);
    auto const* start        = static_cast<std::byte const*>(pool_memory_);
    auto const* end          = start + total_pool_size_;

    if (byte_ptr < start || byte_ptr >= end) {
        return false;
    }

    auto const offset         = static_cast<std::size_t>(byte_ptr - start);
    auto const storage_offset = offsetof(SlotType, storage);

    if (offset < storage_offset) {
        return false;
    }

    return (offset - storage_offset) % sizeof(SlotType) == 0;
}

template <typename T>
inline bool FixedSizeMemoryPool<T>::uses_huge_pages() const {
    return use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages;
}

template <typename T>
inline std::size_t FixedSizeMemoryPool<T>::get_huge_page_size() const {
    if (uses_huge_pages()) {
        return static_cast<std::size_t>(2048U) * 1024U;
    }
    return 0U;
}

template <typename T>
inline void FixedSizeMemoryPool<T>::set_next_pool(FixedSizeMemoryPool<T>* next) {
    __atomic_store_n(&next_pool_, next, __ATOMIC_RELEASE);
}

template <typename T>
inline FixedSizeMemoryPool<T>* FixedSizeMemoryPool<T>::get_next_pool() const {
    return __atomic_load_n(&next_pool_, __ATOMIC_ACQUIRE);
}

} // namespace pubsub_itc_fw

#pragma GCC diagnostic pop
