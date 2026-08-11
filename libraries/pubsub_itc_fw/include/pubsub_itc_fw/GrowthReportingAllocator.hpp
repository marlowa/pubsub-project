#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <memory>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>

namespace pubsub_itc_fw {

/**
 * @file GrowthReportingAllocator.hpp
 * @brief A standard allocator that reports large allocations, for long-lived growing state.
 *
 * The framework's pool and slab allocators cover objects with a message lifecycle: fixed
 * size, allocated and released constantly, and instrumented by
 * handler_for_pool_exhausted so an application can log when the pool has to reach out to
 * the OS. That mechanism works and every component registers a handler for it.
 *
 * It does not cover **long-lived application state that grows** -- an order book, a
 * session table, a subscription registry. Those are typically a hash map or a vector on
 * std::allocator, which means they go straight to the OS heap and are invisible to every
 * instrument the framework provides.
 *
 * Left uninstrumented, such a structure can become the largest consumer of memory in a
 * process while every memory instrument the framework offers reports nothing.
 *
 * This allocator closes that gap without changing anyone's allocation strategy. It
 * delegates to std::allocator and simply reports allocations above a threshold, matching
 * the handler_for_* idiom the pool allocators already use.
 *
 * It suits a growing container particularly well: a hash map or vector allocates its whole
 * storage in **one** call and reallocates on growth, so the callback fires once per
 * doubling and never per element. There is no hot-path cost to speak of -- the check is a
 * comparison against a threshold on an allocation that was already going to the heap.
 *
 * Usage:
 * @code
 *   GrowthReportingAllocator<char>::Reporter book_growth_reporter;
 *   book_growth_reporter.report_threshold_bytes = 64UL * 1024 * 1024;
 *   book_growth_reporter.on_large_allocation = [&logger](size_t bytes, size_t largest) {
 *       PUBSUB_LOG(logger, FwLogLevel::Warning,
 *                  "order book storage grew to {} MB (largest so far {} MB)",
 *                  bytes / (1024 * 1024), largest / (1024 * 1024));
 *   };
 *
 *   tsl::robin_map<Key, Value, Hash, std::equal_to<Key>,
 *                  GrowthReportingAllocator<std::pair<Key, Value>>> book{
 *       0, Hash{}, std::equal_to<Key>{},
 *       GrowthReportingAllocator<std::pair<Key, Value>>{&book_growth_reporter}};
 * @endcode
 *
 * The reporter is owned by the application and must outlive the container. The allocator
 * holds a non-owning pointer to it, because an allocator is copied and rebound freely and
 * every copy must report to the same place.
 *
 * There is no nested rebind struct. std::allocator_traits synthesises the rebound type by
 * substituting the first template argument, which is all this allocator needs, and the nested
 * member it replaced is the C++98 spelling -- deprecated on std::allocator in C++17 and
 * removed in C++20. Every consumer here goes through allocator_traits, tsl::robin_hash
 * included; the converting constructor below is what actually carries the reporter across.
 *
 * A container that owns its storage outright does not need an allocator to report growth at
 * all: it can hold an AllocationGrowthReporter and report its own allocations, which is what
 * IncrementalRehashMap does. This class is for the containers whose storage is not ours --
 * a std::vector, a tsl::robin_map.
 */
template <typename T> class GrowthReportingAllocator {
  public:
    using value_type = T;

    /**
     * @brief Constructs an allocator reporting to @p reporter.
     * @param[in] reporter Non-owning; may be nullptr, in which case nothing is reported.
     */
    explicit GrowthReportingAllocator(AllocationGrowthReporter* reporter = nullptr) : reporter_(reporter) {}

    /// Rebinding constructor: carries the reporter across when a container rebinds this
    /// allocator to its own internal node type.
    ///
    /// DELIBERATELY NOT `explicit`, which deserves justifying because a single-argument
    /// implicit constructor is normally a mistake.
    ///
    /// The Cpp17Allocator requirements specify this as a CONVERTING constructor, and
    /// containers depend on the implicit conversion. `std::allocator` does not mark it
    /// explicit either. "Convert explicitly at the point of need" is not available: the
    /// point of need is inside the container's own headers, which we do not own --
    /// robin_hash.h returns `m_buckets_data.get_allocator()`, of the rebound bucket-entry
    /// type, from a function declared to return the map's own allocator type. There is
    /// nowhere to put a cast. Marking it explicit compiles cleanly until a container
    /// actually uses it, then fails with pages of template diagnostics in third-party code.
    ///
    /// The usual hazard does not arise here. The parameter is another instantiation of
    /// this same template, so nothing unrelated -- an int, a pointer, a string -- can
    /// convert. The constructor that COULD be dangerous, taking a raw
    /// AllocationGrowthReporter*, is explicit and stays that way.
    template <typename U> GrowthReportingAllocator(const GrowthReportingAllocator<U>& other) : reporter_(other.reporter()) {}

    [[nodiscard]] AllocationGrowthReporter* reporter() const {
        return reporter_;
    }

    [[nodiscard]] T* allocate(size_t element_count) {
        const size_t bytes = element_count * sizeof(T);
        if (reporter_ != nullptr && bytes >= reporter_->report_threshold_bytes) {
            // Recorded before the allocation, not after: if this is the allocation that
            // exhausts memory, the report is the last useful thing that happens, and a
            // report published only on success would be lost exactly when it is needed.
            size_t previous = reporter_->largest_allocation_bytes.load(std::memory_order_relaxed);
            while (bytes > previous && !reporter_->largest_allocation_bytes.compare_exchange_weak(previous, bytes, std::memory_order_relaxed)) {}
            if (reporter_->on_large_allocation) {
                reporter_->on_large_allocation(bytes, reporter_->largest_allocation_bytes.load(std::memory_order_relaxed));
            }
        }
        return std::allocator<T>{}.allocate(element_count);
    }

    void deallocate(T* pointer, size_t element_count) {
        std::allocator<T>{}.deallocate(pointer, element_count);
    }

    template <typename U> bool operator==(const GrowthReportingAllocator<U>& other) const {
        return reporter_ == other.reporter();
    }

    template <typename U> bool operator!=(const GrowthReportingAllocator<U>& other) const {
        return !(*this == other);
    }

  private:
    AllocationGrowthReporter* reporter_;
};

} // namespaces
