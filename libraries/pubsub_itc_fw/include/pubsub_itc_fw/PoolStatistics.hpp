#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <cstdint>

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief A struct to hold memory pool statistics.
 *
 * This struct provides a simple, direct way to retrieve key metrics
 * from an `ExpandablePoolAllocator` or `FixedSizeMemoryPool`.
 */
struct PoolStatistics {
    uint64_t total_blocks_allocated = 0;
    uint64_t total_blocks_freed = 0;
    uint64_t current_blocks_in_use = 0;
    uint64_t total_bytes_allocated = 0;
    uint64_t current_bytes_in_use = 0;
};

} // namespace pubsub_itc_fw
