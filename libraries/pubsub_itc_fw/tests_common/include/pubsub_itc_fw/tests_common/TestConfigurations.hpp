#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>

namespace pubsub_itc_fw::tests {

/**
 * @brief Returns a standard QueueConfiguration for use in unit and integration tests.
 *
 * Uses low_watermark=1, high_watermark=64. Tests with unusual queue pressure
 * requirements should construct their own QueueConfiguration directly.
 */
inline QueueConfiguration make_queue_config()
{
    QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

/**
 * @brief Returns a standard AllocatorConfiguration for use in unit and integration tests.
 *
 * @param[in] name The pool name, used to distinguish allocators in log output.
 */
inline AllocatorConfiguration make_allocator_config(const std::string& name)
{
    AllocatorConfiguration cfg{};
    cfg.pool_name        = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

} // namespace pubsub_itc_fw::tests
