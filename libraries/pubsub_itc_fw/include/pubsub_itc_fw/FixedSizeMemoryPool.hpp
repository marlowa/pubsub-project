#pragma once

#include <atomic>      // For std::atomic
#include <cstddef>     // For std::size_t, std::byte
#include <functional>  // For std::function
#include <sstream>     // For std::ostringstream (used by PreconditionAssertion)
#include <stdexcept>   // For std::runtime_error (for huge page errors)
#include <sys/mman.h>  // For mmap, munmap (huge pages)
#include <unistd.h>    // For sysconf(_SC_PAGESIZE) if needed, or get_huge_page_size
#include <string>      // For std::string
#include <algorithm>   // For std::max

#include <pubsub_itc_fw/PreconditionAssertion.hpp>  // For PreconditionAssertion
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>     // For UseHugePagesFlag

// To avoid dependency on Logger in this lock-free core pool,
// we simplify the logging/error handling for huge pages.
// If detailed logging is needed, it should be done by the client (ExpandablePoolAllocator).

namespace pubsub_itc_fw {

/**
 * @brief A fixed-size, lock-free memory pool for objects of type T.
 *
 * This pool pre-allocates a contiguous block of memory at construction time
 * (optionally using huge pages) and manages individual T objects within it.
 * Allocation and deallocation are performed using a lock-free free list
 * based on atomic compare-and-swap (CAS) operations, ensuring thread safety
 * without mutexes for low-latency systems.
 *
 * The pool does not track allocated objects, relying on the caller to
 * correctly `deallocate` objects that were `allocate`d from it.
 *
 * @tparam T The type of object this pool will manage. T must have a default
 * constructor (for placement new) and a destructor.
 */
template <typename T>
class FixedSizeMemoryPool final { // Renamed from Pool to FixedSizeMemoryPool
public:
    /**
     * @brief Destructor for FixedSizeMemoryPool.
     * Calls `clean()` to free the underlying memory block.
     */
    ~FixedSizeMemoryPool() { clean(); }

    /**
     * @brief Constructs a FixedSizeMemoryPool.
     *
     * Pre-allocates a block of memory for `number_of_objects_per_pool` objects.
     * This is the only heap allocation or mmap call performed. The memory
     * blocks are immediately linked into a lock-free free list.
     *
     * @param number_of_objects_per_pool The maximum number of objects this pool can hold.
     * @param use_huge_pages_flag Flag indicating whether to attempt huge page allocation.
     * @param huge_page_size If `use_huge_pages_flag` is `DoUseHugePages`, the size of huge pages in bytes.
     * @param pool_size_rounded_to_huge_page_size If `use_huge_pages_flag` is `DoUseHugePages`, the total pool size
     * rounded up to a huge page multiple.
     * @param handler_for_huge_pages_error Callback invoked if huge page allocation fails, passing `for_client_use`.
     * @param for_client_use A client-defined void pointer passed to callbacks.
     * @throws PreconditionAssertion if `number_of_objects_per_pool` is invalid.
     * @throws std::runtime_error if `mmap` fails to map huge pages and no fallback.
     */
    FixedSizeMemoryPool(int number_of_objects_per_pool, UseHugePagesFlag use_huge_pages_flag,
                        size_t huge_page_size, size_t pool_size_rounded_to_huge_page_size,
                        std::function<void(void*)> handler_for_huge_pages_error,
                        void* for_client_use);

    // Deleted copy operations to ensure unique ownership of the memory block.
    FixedSizeMemoryPool(const FixedSizeMemoryPool&) = delete;
    FixedSizeMemoryPool& operator=(const FixedSizeMemoryPool&) = delete;

    // Deleted move operations as pools are typically long-lived and non-movable.
    FixedSizeMemoryPool(FixedSizeMemoryPool&&) = delete;
    FixedSizeMemoryPool& operator=(FixedSizeMemoryPool&&) = delete;

    /**
     * @brief Allocates and constructs an object of type T from the pool in a lock-free manner.
     *
     * If the pool is exhausted, it returns `nullptr`. The allocated object is
     * constructed using its default constructor via placement new.
     *
     * @return A pointer to the newly allocated and constructed object, or `nullptr` if the pool is full.
     */
    [[nodiscard]] T* allocate();

    /**
     * @brief Deallocates an object, returning its memory to the pool in a lock-free manner.
     *
     * The object's destructor is explicitly called before its memory is returned.
     * The caller *must* ensure that `object_to_deallocate` was originally
     * `allocate`d from this pool.
     *
     * @param object_to_deallocate A pointer to the object to deallocate.
     */
    void deallocate(T* object_to_deallocate);

    /**
     * @brief Checks if a pointer belongs to this pool.
     */
    bool contains(T* object_to_check) const {
        return (reinterpret_cast<std::byte*>(object_to_check) >= reinterpret_cast<std::byte*>(start_of_pool_)) &&
               (reinterpret_cast<std::byte*>(object_to_check) < (reinterpret_cast<std::byte*>(start_of_pool_) + (number_of_objects_per_pool_ * sizeof(T))));
    }

    /**
     * @brief Frees the underlying memory block.
     * All objects currently allocated from this pool are destroyed.
     * This method is NOT thread-safe for concurrent allocations/deallocations.
     * It should only be called when the pool is no longer in use or during teardown.
     */
    void clean();

    /**
     * @brief Returns the flag indicating whether huge pages are in use by this pool.
     * @return The `UseHugePagesFlag` enum value.
     */
    [[nodiscard]] UseHugePagesFlag get_use_huge_pages_flag() const { return use_huge_pages_flag_; }

private:
    // Helper to push a node onto the lock-free free list head
    void push_node_to_free_list(T* node_to_push);

    // Helper to pop a node from the lock-free free list head
    [[nodiscard]] T* pop_node_from_free_list();

    UseHugePagesFlag use_huge_pages_flag_;             /**< @brief Flag for huge pages usage. */
    size_t huge_page_size_{0};                         /**< @brief Size of huge pages if used. */
    size_t pool_size_rounded_to_huge_page_size_{0};    /**< @brief Actual size of huge page memory block. */
    std::function<void(void*)> handler_for_huge_pages_error_; /**< @brief Callback for huge pages allocation error. */
    void* for_client_use_{nullptr};                    /**< @brief Client-defined data for callbacks. */
    T* start_of_pool_{nullptr};                        /**< @brief Pointer to the start of the allocated memory block. */
    int number_of_objects_per_pool_{0};                /**< @brief Total objects this pool can hold. */

    // Head of the lock-free free list of available nodes.
    // Each T object needs an intrusive 'next' pointer for this free list.
    // This implies that T itself must be designed to contain such a pointer,
    // or we store raw `std::byte` blocks and manage `T*` pointers.
    // For simplicity, we assume T can be treated as a raw block that can be cast to T*.
    // The nodes in the free list are raw pointers to the T objects within the pool.
    // We'll require that T is 'intrusive' (or we manage a separate intrusive node wrapper).
    // Given previous MpscNode, T could contain `std::atomic<T*>` for the free list.
    // However, a simpler approach for a general FixedSizeMemoryPool is to manage raw memory
    // blocks and keep the free list pointers separate, or assume T is a `union` or similar.
    // A more practical lock-free free list for generic T typically needs T to *not* be the node itself.
    // Let's refine: the free list manages `void*` or `std::byte*` pointers to raw memory,
    // and those raw blocks are reinterpreted as `T*`.
    // The "next" pointer for the free list must be stored *within* the free memory block itself,
    // which implies that the `T` object (when not allocated) has its memory used for linking.
    // This is a standard lock-free free list pattern.

    // A type that represents a node in the free list. It takes over the memory of T when T is not allocated.
    struct FreeListNode {
        std::atomic<FreeListNode*> next;
        // The remaining space in this struct's memory is for the T object.
        // It's essential that sizeof(FreeListNode) <= sizeof(T)
        // or that FreeListNode is carefully laid out within T's memory.
        // For simplicity, we assume T's memory can be used for the next pointer.
    };
    static_assert(sizeof(FreeListNode) <= sizeof(T), "T must be large enough to embed FreeListNode for the intrusive free list.");

    alignas(std::atomic<T*>)
    std::atomic<T*> free_list_head_ptr_; /**< @brief Head of the lock-free free list. */
};


// --- FixedSizeMemoryPool Method Implementations ---

template <typename T>
FixedSizeMemoryPool<T>::FixedSizeMemoryPool(int number_of_objects_per_pool,
                                             UseHugePagesFlag use_huge_pages_flag,
                                             size_t huge_page_size, size_t pool_size_rounded_to_huge_page_size,
                                             std::function<void(void*)> handler_for_huge_pages_error,
                                             void* for_client_use)
    : use_huge_pages_flag_(use_huge_pages_flag),
      huge_page_size_(huge_page_size),
      pool_size_rounded_to_huge_page_size_(pool_size_rounded_to_huge_page_size),
      handler_for_huge_pages_error_(std::move(handler_for_huge_pages_error)),
      for_client_use_(for_client_use),
      start_of_pool_(nullptr),
      number_of_objects_per_pool_(number_of_objects_per_pool),
      free_list_head_ptr_(nullptr) // Initialize atomic pointer
{
    if (number_of_objects_per_pool_ <= 0) {
        std::ostringstream output_stream;
        output_stream << "FixedSizeMemoryPool ctor, invalid number of objects for pool [" << number_of_objects_per_pool_ << "]";
        throw PreconditionAssertion(output_stream.str(), __FILE__, __LINE__);
    }

    size_t required_size = static_cast<size_t>(number_of_objects_per_pool_) * sizeof(T);

    if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
        // Use mmap with MAP_HUGETLB
        void* ptr = mmap(nullptr, pool_size_rounded_to_huge_page_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            // Huge page allocation failed, fall back to normal allocation if desired, or throw.
            // As per original logic, change flag and call error handler.
            use_huge_pages_flag_ = UseHugePagesFlag::DoNotUseHugePages;
            if (handler_for_huge_pages_error_ != nullptr) {
                handler_for_huge_pages_error_(for_client_use_);
            }
            start_of_pool_ = static_cast<T*>(::operator new(required_size));
            // No logging here, client (ExpandablePoolAllocator) is responsible for logging.
        } else {
            start_of_pool_ = reinterpret_cast<T*>(ptr);
            // No logging here.
        }
    } else {
        // Normal heap allocation
        start_of_pool_ = static_cast<T*>(::operator new(required_size));
    }

    // Initialize the lock-free free list
    for (int i = 0; i < number_of_objects_per_pool_; ++i) {
        T* object_ptr = &start_of_pool_[i];
        push_node_to_free_list(object_ptr);
    }
}

template <typename T>
void FixedSizeMemoryPool<T>::clean() {
    // Note: This method is NOT thread-safe for concurrent allocate/deallocate.
    // It should only be called when the pool is no longer in use.
    if (start_of_pool_ != nullptr) {
        // Objects currently in use are NOT destructed here.
        // We only destroy objects that were placed back into the free list
        // by explicitly calling their destructors during deallocate.
        // If an object is still "allocated" by a client, its destructor is
        // the client's responsibility or relies on its unique_ptr/shared_ptr.

        if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
            munmap(start_of_pool_, pool_size_rounded_to_huge_page_size_);
        } else {
            ::operator delete(start_of_pool_);
        }
        start_of_pool_ = nullptr;
        free_list_head_ptr_.store(nullptr, std::memory_order_release); // Clear free list head
    }
}

template <typename T>
T* FixedSizeMemoryPool<T>::allocate() {
    T* object_ptr = pop_node_from_free_list();
    if (object_ptr != nullptr) {
        // Placement new to construct the object
        new (object_ptr) T(); // Uses T's default constructor
    }
    return object_ptr;
}

template <typename T>
void FixedSizeMemoryPool<T>::deallocate(T* object_to_deallocate) {
    if (object_to_deallocate == nullptr) {
        return; // Cannot deallocate a null pointer
    }
    if (!contains(object_to_deallocate)) {
        throw PreconditionAssertion("Attempted to deallocate a pointer that does not belong to this memory pool.", __FILE__, __LINE__);
    }

    // Explicitly call the destructor of the object.
    object_to_deallocate->~T();
    // Push the raw memory block back to the free list.
    push_node_to_free_list(object_to_deallocate);
}

template <typename T>
void FixedSizeMemoryPool<T>::push_node_to_free_list(T* node_to_push) {
    // For intrusive free list, we use the beginning of the T object's memory
    // to store the 'next' pointer when it's on the free list.
    // This requires T to be large enough to hold a pointer.
    // The `FreeListNode` struct helps clarify this.
    FreeListNode* free_list_node = reinterpret_cast<FreeListNode*>(node_to_push);

    T* old_head = free_list_head_ptr_.load(std::memory_order_relaxed);
    do {
        free_list_node->next.store(reinterpret_cast<FreeListNode*>(old_head), std::memory_order_relaxed);
    } while (!free_list_head_ptr_.compare_exchange_weak(
                                            old_head,
                                            node_to_push,
                                            std::memory_order_release,
                                            std::memory_order_relaxed));
}

template <typename T>
T* FixedSizeMemoryPool<T>::pop_node_from_free_list() {
    T* old_head = free_list_head_ptr_.load(std::memory_order_acquire);
    while (old_head != nullptr && !free_list_head_ptr_.compare_exchange_weak(
                                            old_head,
                                            reinterpret_cast<T*>(reinterpret_cast<FreeListNode*>(old_head)->next.load(std::memory_order_relaxed)),
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        // Spin until CAS succeeds or another thread acquires the last node.
        // The value of old_head is updated by compare_exchange_weak on failure.
    }
    return old_head; // Returns nullptr if pool was empty or became empty
}


} // namespace pubsub_itc_fw
