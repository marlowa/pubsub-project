#pragma once

#include <cstdint> // For std::uint32_t
#include <fmt/core.h>
#include <iostream>
#include <string> // For std::string
#include <vector> // For std::vector

#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

/**
 * @brief Represents a snapshot of the current state and metrics of a memory pool.
 *
 * This struct is designed to be a Plain Old Data (POD) type or a simple aggregate,
 * allowing it to be easily copied and used for reporting. It holds various
 * statistics about the allocator's pools, such as object size, counts, and
 * huge page usage.
 */
struct PoolStatistics {
    // Basic Info
    std::string pool_name_;                                                     /**< @brief The name of the pool allocator. */
    UseHugePagesFlag use_huge_pages_flag_{UseHugePagesFlag::DoNotUseHugePages}; /**< @brief Flag indicating if huge pages are in use. */
    size_t huge_page_size_{0};                                                  /**< @brief The size of huge pages, if used. */
    size_t object_size_{0};                                                     /**< @brief The size of the objects managed by the pool. */

    // Pool Metrics
    int number_of_objects_per_pool_{0}; /**< @brief The number of objects each pool can hold. */
    int number_of_pools_{0};            /**< @brief The total number of memory pools currently allocated. */
    int number_of_huge_page_pools_{0};  /**< @brief The number of pools using huge pages. */

    // Allocation Metrics (may be approximate in a concurrent environment)
    int number_of_allocated_objects_{0}; /**< @brief The total number of objects currently allocated from all pools. */
    int number_of_objects_available_{0}; /**< @brief The total number of objects available for allocation across all pools. */
    int number_of_full_pools_{0};        /**< @brief The number of pools that are completely full. */

    /**
     * @brief Logs the pool statistics to a provided `LoggerInterface`.
     * @param logger The logger to use for output.
     */
    void log_statistics(QuillLogger& logger) const {
        PUBSUB_LOG(logger, LogLevel::Info, "--- Pool Statistics for '{}' ---", pool_name_);
        PUBSUB_LOG(logger, LogLevel::Info, "Object Size: {} bytes", object_size_);
        PUBSUB_LOG(logger, LogLevel::Info, "Objects Per Pool: {}", number_of_objects_per_pool_);
        PUBSUB_LOG(logger, LogLevel::Info, "Total Pools: {}", number_of_pools_);
        PUBSUB_LOG(logger, LogLevel::Info, "Huge Pages In Use: {}", (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages ? "Yes" : "No"));
        if (use_huge_pages_flag_ == UseHugePagesFlag::DoUseHugePages) {
            PUBSUB_LOG(logger, LogLevel::Info, "  Huge Page Size: {} bytes", huge_page_size_);
            PUBSUB_LOG(logger, LogLevel::Info, "  Huge Page Pools: {}", number_of_huge_page_pools_);
        }
        PUBSUB_LOG(logger, LogLevel::Info, "Allocated Objects: {}", number_of_allocated_objects_);
        PUBSUB_LOG(logger, LogLevel::Info, "Available Objects: {}", number_of_objects_available_);
        PUBSUB_LOG(logger, LogLevel::Info, "Full Pools: {}", number_of_full_pools_);
        // Fix for the C++11 warning: Pass a dummy empty string for the variadic argument.
        PUBSUB_LOG(logger, LogLevel::Info, "-------------------------------------", "");
    }
};

} // namespace pubsub_itc_fw
