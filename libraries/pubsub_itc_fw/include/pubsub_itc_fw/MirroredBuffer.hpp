#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A stream-oriented ring buffer using virtual memory mirroring.
 *
 * @ingroup memory_subsystem
 *
 * This class provides a contiguous view of a byte stream, even when the data wraps
 * around the end of the physical buffer. It achieves this by mapping the same
 * physical memory to two adjacent virtual address ranges.
 *
 * This is intended for "Alien" protocols like ASCII FIX, allowing the Reactor
 * to perform a single read() and the Application to perform in-place parsing
 * without handling split-packet edge cases.
 *
 * Threading Model:
 * - The Reactor thread is the sole Producer (calls write_ptr and advance_head).
 * - The Application thread is the sole Consumer (calls read_ptr and advance_tail).
 * - Note: Synchronisation of the head and tail indices must be managed via
 * the external control block or event messages.
 */
class MirroredBuffer {
public:

    /**
     * @brief Unmaps the virtual address ranges and closes the underlying file descriptor.
     */
    ~MirroredBuffer();

    /**
     * @brief Initialises the mirrored buffer with a double-mapped virtual address range.
     *
     * @param requested_capacity The minimum physical size of the buffer in bytes.
     * Will be rounded up to the nearest system page size.
     *
     * @throw PreconditionAssertion If requested_capacity is non-positive.
     * @throw PubSubItcException If system calls (memfd_create, mmap, ftruncate) fail.
     */
    explicit MirroredBuffer(int64_t requested_capacity);

    /**
     * @brief Returns the total physical capacity of the buffer.
     * @return Capacity in bytes (page-aligned).
     */
    [[nodiscard]] int64_t capacity() const;

    /**
     * @brief Returns the current write position for the Reactor.
     *
     * Because of the mirroring trick, there are always at least (capacity - bytes_available)
     * contiguous bytes available for writing starting from this pointer.
     *
     * @return A pointer to the next byte to be written.
     */
    [[nodiscard]] uint8_t* write_ptr();

    /**
     * @brief Updates the head index after data has been written to the buffer.
     *
     * @param bytes The number of bytes successfully written.
     * @throw PreconditionAssertion If bytes is negative or exceeds space_remaining().
     */
    void advance_head(int64_t bytes);

    /**
     * @brief Returns the current read position for the Application.
     *
     * Because of the mirroring trick, there are always 'bytes_available'
     * contiguous bytes available for reading starting from this pointer.
     *
     * @return A pointer to the next byte to be consumed.
     */
    [[nodiscard]] const uint8_t* read_ptr() const;

    /**
     * @brief Updates the tail index after data has been consumed from the buffer.
     *
     * @param bytes The number of bytes successfully processed.
     * @throw PreconditionAssertion If bytes is negative or exceeds bytes_available().
     */
    void advance_tail(int64_t bytes);

    /**
     * @brief Returns the current tail index.
     *
     * Used by RawBytesProtocolHandler to stamp each RawSocketCommunication
     * EventMessage with the tail position at enqueue time. The application
     * thread compares this against its last seen tail to detect unambiguously
     * whether the tail advanced between deliveries.
     *
     * @return Current tail position in bytes.
     */
    [[nodiscard]] int64_t tail() const { return tail_; }

    /**
     * @brief Calculates the number of bytes currently held in the buffer.
     * @return Number of bytes available to read.
     */
    [[nodiscard]] int64_t bytes_available() const;

    /**
     * @brief Calculates the remaining writable space.
     *
     * Consistent with standard ring buffer practice, one byte is left unused
     * to distinguish between the 'full' and 'empty' states.
     *
     * @return Number of bytes available to write.
     */
    [[nodiscard]] int64_t space_remaining() const;

private:
    /**
     * @brief Rounds the requested size up to the system page size.
     * @param size The raw size requested.
     * @return The aligned size.
     */
    [[nodiscard]] static int64_t round_to_page_size(int64_t size);

    uint8_t* base_ptr_{nullptr};
    int64_t  capacity_{0};
    int64_t  head_{0};
    int64_t  tail_{0};
    int      shm_fd_{-1};
};

} // namespace pubsub_itc_fw
