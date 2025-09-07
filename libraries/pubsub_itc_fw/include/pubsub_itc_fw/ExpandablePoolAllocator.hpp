#pragma once

// C headers including posix API headers
// (None directly here)

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <memory>
#include <mutex>
#include <vector>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>

namespace pubsub_itc_fw {

/**
 * @brief An expandable memory allocator composed of fixed-size pools.
 *
 * This class manages a collection of `FixedSizeMemoryPool` instances and
 * provides a thread-safe allocation and deallocation interface. If a pool
 * fills up, it can allocate a new one without a set limit. It is designed to
 * continue creating new pools until the system's memory is exhausted.
 */
class ExpandablePoolAllocator final {
  public:
    // RAII for resource management
    ~ExpandablePoolAllocator();

    /**
     * @brief Constructs an ExpandablePoolAllocator.
     * @param [in] initial_block_size The size of blocks in the initial pool.
     * @param [in] initial_num_blocks The number of blocks in the initial pool.
     */
    ExpandablePoolAllocator(size_t initial_block_size, size_t initial_num_blocks);

    /**
     * @brief Allocates a block of memory of the specified size.
     * @param [in] size The size of the memory block to allocate.
     * @return A raw pointer to the allocated memory block, or `nullptr` if allocation fails.
     */
    [[nodiscard]] void* allocate(size_t size);

    /**
     * @brief Deallocates a block of memory.
     * @param [in] ptr The pointer to the memory block to deallocate.
     */
    void deallocate(void* ptr);

    /**
     * @brief Returns the current statistics for the memory pool.
     * @return A `PoolStatistics` struct containing the current metrics.
     */
    [[nodiscard]] PoolStatistics get_statistics() const;

  private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<FixedSizeMemoryPool>> pools_;
    size_t initial_block_size_;
    size_t initial_num_blocks_;
    PoolStatistics stats_;
};

} // namespace pubsub_itc_fw
