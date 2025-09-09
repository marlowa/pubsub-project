#pragma once

namespace pubsub_itc_fw {

/**
 * @brief Enumeration to indicate whether huge pages should be used for memory allocation.
 *
 * Huge pages can improve performance for applications that require large amounts
 * of contiguous memory by reducing Translation Lookaside Buffer (TLB) misses.
 */
enum class UseHugePagesFlag { // Renamed file to UseHugePagesFlag.hpp
    DoNotUseHugePages, /**< @brief Do not use huge pages for memory allocation. */
    DoUseHugePages     /**< @brief Attempt to use huge pages for memory allocation. */
};

} // namespace pubsub_itc_fw
