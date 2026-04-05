#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * BumpAllocator is a non-owning bump allocator over a caller-supplied byte buffer.
 * It supports typed allocation of objects of arbitrary type into a contiguous
 * scratch region, advancing an internal cursor on each allocation. All allocations
 * share the same lifetime and are released together by calling reset().
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
 *
 * Sizing:
 *   For decode contexts, the generated header for each DSL message type provides
 *   a max_decode_arena_bytes_<MessageName>() helper that returns a conservative
 *   upper bound on the number of bytes required to decode one message of that
 *   type given a maximum number of elements per list.
 *
 *   For encode contexts, a corresponding max_encode_arena_bytes_<MessageName>()
 *   helper is provided for the same purpose.
 *
 *   Use these helpers to size the backing buffer at the declaration site.
 */

#include <cstddef>
#include <cstdint>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Non-owning bump allocator over a caller-supplied byte buffer.
 *
 * Provides typed allocation without heap allocation. All allocations are
 * released together by calling reset(). Intended for use as a short-lived
 * scratch allocator in DSL message encode and decode operations.
 *
 * Name the instance to reflect its use context:
 *   BumpAllocator decode_arena  -- when used during message decoding
 *   BumpAllocator encode_arena  -- when used during message encoding
 */
class BumpAllocator {
public:
    /**
     * @brief Constructs a BumpAllocator over a caller-supplied buffer.
     *
     * @param[in] storage Pointer to the backing byte buffer. Must not be nullptr.
     *                    Must remain valid for the lifetime of this BumpAllocator.
     * @param[in] capacity Size of the backing buffer in bytes. Must be greater than zero.
     */
    BumpAllocator(uint8_t* storage, std::size_t capacity)
        : storage_(storage)
        , capacity_(capacity)
        , bytes_used_(0) {
        if (storage_ == nullptr) {
            throw PreconditionAssertion("BumpAllocator: storage must not be nullptr", __FILE__, __LINE__);
        }
        if (capacity_ == 0) {
            throw PreconditionAssertion("BumpAllocator: capacity must be greater than zero", __FILE__, __LINE__);
        }
    }

    ~BumpAllocator() = default;

    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;

    /**
     * @brief Allocates storage for element_count objects of type T.
     *
     * Aligns the internal cursor to alignof(T), then advances it by
     * sizeof(T) * element_count. Returns nullptr if the allocator does not
     * have sufficient remaining capacity. The caller must treat a nullptr
     * return as a failure and propagate it accordingly.
     *
     * No constructors are called on the returned memory. The caller is
     * responsible for constructing objects into the returned region via
     * placement new or direct assignment.
     *
     * @param[in] element_count Number of T objects to allocate space for.
     *                          Must be greater than zero.
     * @return Pointer to aligned storage for element_count T objects,
     *         or nullptr if capacity is exhausted.
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

        if (required_bytes > capacity_) {
            return nullptr;
        }

        bytes_used_ = required_bytes;
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
     * @brief Returns the number of bytes currently allocated from this allocator.
     *
     * @return Bytes allocated since the last reset().
     */
    [[nodiscard]] std::size_t bytes_used() const {
        return bytes_used_;
    }

    /**
     * @brief Returns the total capacity of this allocator in bytes.
     *
     * @return Total capacity of the backing buffer.
     */
    [[nodiscard]] std::size_t bytes_capacity() const {
        return capacity_;
    }

    /**
     * @brief Returns the number of bytes still available for allocation.
     *
     * @return Remaining capacity in bytes.
     */
    [[nodiscard]] std::size_t bytes_remaining() const {
        return capacity_ - bytes_used_;
    }

private:
    uint8_t* storage_;
    std::size_t capacity_;
    std::size_t bytes_used_;
};

} // namespace pubsub_itc_fw
