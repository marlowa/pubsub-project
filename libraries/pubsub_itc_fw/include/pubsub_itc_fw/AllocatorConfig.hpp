#pragma once

#include <functional>
#include <string>

#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

class AllocatorConfig
{
public:
    std::string pool_name;

    // Pool sizing
    int objects_per_pool{0};
    int initial_pools{0};
    int expansion_threshold_hint{0};

    // Error / event handlers
    std::function<void(void* context, int objects_requested)> handler_for_pool_exhausted;
    std::function<void(void* context, void* invalid_ptr)> handler_for_invalid_free;
    std::function<void(void* context)> handler_for_huge_pages_error;

    // Huge page usage
    UseHugePagesFlag use_huge_pages_flag{UseHugePagesFlag::DoNotUseHugePages};

    // Optional context pointer for allocator callbacks
    void* context{nullptr};

    AllocatorConfig() = default;
};

} // namespaces
