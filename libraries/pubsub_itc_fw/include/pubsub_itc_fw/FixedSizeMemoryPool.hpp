#pragma once

// C headers including posix API headers
#include <stddef.h> // For size_t

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <memory>
#include <vector>

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief A memory pool that manages fixed-size blocks of memory.
 *
 * This is a low-level, high-performance allocator that provides blocks of a
 * consistent size. It's intended to be used as a building block for a more
 * flexible allocator like `ExpandablePoolAllocator`.
 */
class FixedSizeMemoryPool final {
  public:
    /**
     * @brief Constructs a fixed-size memory pool.
     * @param [in] block_size The size of each memory block in bytes.
     * @param [in] num_blocks The total number of blocks in the pool.
     */
    FixedSizeMemoryPool(size_t block_size, size_t num_blocks);

    // RAII for resource management
    ~FixedSizeMemoryPool();

    /**
     * @brief Allocates a block of memory from the pool.
     * @return A raw pointer to the allocated memory block, or `nullptr` if the pool is full.
     */
    [[nodiscard]] void* allocate();

    /**
     * @brief Deallocates a block of memory, returning it to the pool.
     * @param [in] ptr The pointer to the memory block to deallocate.
     */
    void deallocate(void* ptr);

    /**
     * @brief Returns the size of each memory block in the pool.
     * @return The block size in bytes.
     */
    [[nodiscard]] size_t get_block_size() const;

    /**
     * @brief Returns the total number of blocks in the pool.
     * @return The total number of blocks.
     */
    [[nodiscard]] size_t get_total_blocks() const;

    /**
     * @brief Returns the number of currently allocated blocks.
     * @return The number of allocated blocks.
     */
    [[nodiscard]] size_t get_allocated_blocks() const;

  private:
    size_t block_size_;
    size_t total_blocks_;
    size_t allocated_blocks_;
    std::vector<char> memory_;
    std::vector<void*> free_list_;
};

} // namespace pubsub_itc_fw
