#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * BumpAllocator is a non-owning bump allocator over a caller-supplied byte buffer.
 * It supports typed allocation of objects of arbitrary type into a contiguous
 * scratch region, advancing an internal cursor on each allocation. All allocations
 * share the same lifetime and are released together by calling reset().
 *
 * snprintf contract:
 *   BumpAllocator follows the same contract as snprintf:
 *
 *   - When constructed with a real buffer, allocate<T>() returns a pointer into
 *     that buffer and advances the cursor. If the buffer is too small,
 *     allocate<T>() returns nullptr but bytes_used() still reflects the number
 *     of bytes that WOULD have been needed. The caller can use this to size a
 *     larger buffer and retry, exactly as one would with snprintf.
 *
 *   - When constructed with nullptr and capacity zero (the "measuring" mode),
 *     allocate<T>() always returns nullptr but advances bytes_used() as if
 *     allocation had succeeded. This allows the caller to perform a dry run to
 *     discover exactly how many bytes are needed before allocating any real
 *     storage -- again, exactly as with snprintf:
 *
 *       // snprintf equivalent: passing nullptr/0 to measure required size
 *       BumpAllocator arena(nullptr, 0);
 *
 *   Typical two-pass pattern for variable-length encode:
 *
 *     // Pass 1: measure
 *     BumpAllocator measuring_arena(nullptr, 0);
 *     encode(message, wire_buffer, measuring_arena);
 *     std::size_t needed = measuring_arena.bytes_used();
 *
 *     // Pass 2: allocate real storage and retry
 *     auto [slab_id, ptr] = slab_allocator.allocate(needed);
 *     BumpAllocator real_arena(static_cast<uint8_t*>(ptr), needed);
 *     encode(message, wire_buffer, real_arena);
 *
 *   For fixed-size messages (encode_fast), no arena is needed at all.
 *
 *   For the common case where the maximum list size is known in advance, use the
 *   DSL-generated max_encode_arena_bytes_<MessageName>(max_elements) or
 *   max_decode_arena_bytes_<MessageName>(max_elements) helpers to pre-size a
 *   stack buffer and avoid the two-pass pattern entirely. These helpers return
 *   a conservative upper bound for a given maximum number of list elements. If
 *   at runtime the actual data fits within that bound, no two-pass retry is needed.
 *
 * Typical uses within the pubsub_itc_fw DSL layer:
 *
 *   Decode context (variable named decode_arena):
 *     Provides scratch storage for view structs (ListView<T>, std::string_view,
 *     generated XxxView structs) during DSL message decoding. The caller resets
 *     the allocator before each decode and the decoded view remains valid until
 *     the next reset.
 *
 *   Encode context (variable named encode_arena):
 *     Provides scratch storage for building ListView data arrays before encoding
 *     a DSL message. The caller resets the allocator after encode is complete.
 *
 * Ownership model:
 *   The caller supplies the backing buffer and is responsible for its lifetime.
 *   BumpAllocator does not allocate, own, or free any memory.
 *
 * Lifetime model:
 *   All allocations made from a BumpAllocator share the same lifetime. There is
 *   no individual deallocation. The caller calls reset() when the allocated
 *   region is no longer needed.
 *
 * Thread safety:
 *   BumpAllocator is not thread-safe. Each thread must own its own instance.
 *   A BumpAllocator must never be shared across threads.
 */

#include <cstddef>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Non-owning bump allocator over a caller-supplied byte buffer.
 *
 * Follows the snprintf contract: allocate() always advances bytes_used() by
 * the number of bytes that would be required, whether or not a real buffer
 * was supplied and whether or not capacity was sufficient. When storage is
 * nullptr or capacity is exhausted, allocate() returns nullptr. This allows
 * a two-pass pattern -- measure with nullptr, then allocate real storage and
 * retry -- without any heap allocation.
 *
 * Name the instance to reflect its use context:
 *   BumpAllocator decode_arena  -- when used during message decoding
 *   BumpAllocator encode_arena  -- when used during message encoding
 */
class BumpAllocator {
public:
    /**
     * @brief Constructs a BumpAllocator.
     *
     * Pass a real buffer and its size for normal allocation. Pass nullptr and
     * zero to use measuring mode, where allocate() always returns nullptr but
     * bytes_used() accurately reflects the bytes that would have been needed.
     * This is the same contract as snprintf(nullptr, 0, ...).
     *
     * @param[in] storage  Pointer to the backing byte buffer, or nullptr for
     *                     measuring mode. If non-null, must remain valid for
     *                     the lifetime of this BumpAllocator.
     * @param[in] capacity Size of the backing buffer in bytes, or zero for
     *                     measuring mode.
     */
    BumpAllocator(uint8_t* storage, std::size_t capacity)
        : storage_(storage)
        , capacity_(capacity)
        , bytes_used_(0) {
    }

    ~BumpAllocator() = default;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;

    /**
     * @brief Allocates storage for element_count objects of type T.
     *
     * Always advances bytes_used() by the aligned size required, regardless
     * of whether allocation succeeds. This mirrors the snprintf contract:
     * the return value indicates success or failure, but bytes_used() always
     * reflects the true bytes needed.
     *
     * When storage is nullptr or remaining capacity is insufficient, returns
     * nullptr. The caller may then inspect bytes_used() to determine how large
     * a real buffer must be, allocate it, and retry.
     *
     * No constructors are called on the returned memory. The caller is
     * responsible for constructing objects into the returned region via
     * placement new or direct assignment.
     *
     * @param[in] element_count Number of T objects to allocate space for.
     *                          Must be greater than zero.
     * @return Pointer to aligned storage for element_count T objects,
     *         or nullptr if storage is nullptr or capacity is exhausted.
     */
    template<typename T>
    [[nodiscard]] T* allocate(std::size_t element_count) {
        if (element_count == 0) {
            throw PreconditionAssertion(
                "BumpAllocator::allocate: element_count must be greater than zero",
                __FILE__, __LINE__);
        }

        constexpr std::size_t alignment = alignof(T);
        std::size_t aligned_offset = (bytes_used_ + alignment - 1) & ~(alignment - 1);
        std::size_t required_bytes = aligned_offset + sizeof(T) * element_count;

        bytes_used_ = required_bytes;

        if (storage_ == nullptr || required_bytes > capacity_) {
            return nullptr;
        }

        return reinterpret_cast<T*>(storage_ + aligned_offset);
    }

    /**
     * @brief Resets the allocator, making all previously allocated storage
     *        available again.
     *
     * Does not zero the backing buffer. Must be called before each new
     * encode or decode operation.
     */
    void reset() {
        bytes_used_ = 0;
    }

    /**
     * @brief Returns the number of bytes that have been (or would have been)
     *        allocated since the last reset().
     *
     * In measuring mode (storage is nullptr), this reflects the true bytes
     * that would have been needed, even though no real allocation occurred.
     *
     * @return Bytes used or required since the last reset().
     */
    [[nodiscard]] std::size_t bytes_used() const {
        return bytes_used_;
    }

    /**
     * @brief Returns the total capacity of this allocator in bytes.
     *
     * Returns zero when in measuring mode.
     *
     * @return Total capacity of the backing buffer.
     */
    [[nodiscard]] std::size_t bytes_capacity() const {
        return capacity_;
    }

    /**
     * @brief Returns the number of bytes still available for allocation.
     *
     * Returns zero when in measuring mode or when capacity is exhausted.
     *
     * @return Remaining capacity in bytes.
     */
    [[nodiscard]] std::size_t bytes_remaining() const {
        if (bytes_used_ >= capacity_) {
            return 0;
        }
        return capacity_ - bytes_used_;
    }

    /**
     * @brief Returns true if this allocator is in measuring mode.
     *
     * Measuring mode is active when storage is nullptr. In this mode,
     * allocate() always returns nullptr but bytes_used() accurately
     * reflects the bytes that would have been needed.
     */
    [[nodiscard]] bool is_measuring() const {
        return storage_ == nullptr;
    }

private:
    uint8_t* storage_;
    std::size_t capacity_;
    std::size_t bytes_used_;
};

} // namespace pubsub_itc_fw
