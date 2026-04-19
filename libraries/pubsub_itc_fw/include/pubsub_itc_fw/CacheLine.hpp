#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <type_traits>

namespace pubsub_itc_fw {

/** @ingroup allocator_subsystem */

/**
 * @brief A wrapper that places a value on its own hardware cache line.
 *
 * Modern x86‑64 processors use a 64‑byte cache line. When two frequently
 * modified variables share the same cache line, concurrent threads can
 * cause *false sharing*: the cache line bounces between cores even though
 * the threads are modifying different variables. This can severely degrade
 * performance in highly concurrent code.
 *
 * CacheLine<T> ensures that the wrapped value occupies an entire cache line
 * by itself. Adjacent CacheLine<T> instances will not share a cache line,
 * eliminating false sharing between them.
 *
 * Typical use cases include:
 *   - high‑frequency atomic counters (e.g. statistics, metrics)
 *   - shared state that is written by one thread and read by others
 *   - any variable that is updated often enough to risk false sharing
 *
 * @tparam T The type to wrap. Usually an atomic or a small trivially
 *           copyable type such as a pointer or integer.
 *
 * @note The struct is aligned to 64 bytes and padded to exactly one cache line.
 *       This does not change the semantics of T; it only affects placement.
 */
template <typename T>
struct alignas(64) CacheLine {
    static_assert(std::is_trivially_copyable_v<T>,
                  "CacheLine<T> requires T to be trivially copyable.");

    T value{};  // zero-initialised, so decls of CacheLine variables do not need braces

    CacheLine() = default;
    CacheLine(const CacheLine&) = default;
    CacheLine& operator=(const CacheLine&) = default;
};

static_assert(sizeof(CacheLine<std::atomic<uint64_t>>) >= 64 &&
              sizeof(CacheLine<std::atomic<uint64_t>>) % 64 == 0,
              "CacheLine must occupy at least one full cache line.");

} // namespace pubsub_itc_fw
