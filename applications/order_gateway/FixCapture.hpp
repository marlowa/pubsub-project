#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace order_gateway {

/**
 * @brief Captures inbound and outbound FIX wire bytes to a binary file.
 *
 * The gateway thread calls capture() with no file I/O: the record is packed
 * into a pre-allocated SPSC ring buffer.  A background writer thread drains
 * the ring and writes to disk.
 *
 * Binary record format (little-endian):
 *   uint32_t  payload_size    -- byte count of the raw FIX data
 *   int64_t   timestamp_ns    -- nanoseconds since Unix epoch (wall clock)
 *   uint8_t   direction       -- 0 = inbound, 1 = outbound
 *   uint8_t   data[...]       -- raw FIX wire bytes
 *
 * Ring buffer layout:
 *   Records are packed end-to-end, each padded to 4-byte alignment.  When a
 *   record would cross the end of the ring a sentinel record (payload_size ==
 *   SENTINEL) is written at the current write position and the write pointer
 *   wraps to the start.  The reader recognises the sentinel and wraps its own
 *   read pointer identically.  This means a record is always stored linearly
 *   (never split across the wrap boundary), so the writer thread can pass a
 *   direct pointer into the ring to fwrite() without copying.
 *
 * The ring buffer is allocated once at construction (no per-record heap
 * allocation) so capture() never calls malloc and the writer thread never
 * calls free — cross-thread allocator contention is eliminated entirely.
 *
 * The ring capacity is specified as a byte count at construction.  If the
 * writer thread falls behind and the ring fills, capture() drops the record
 * and logs a Warning.
 *
 * Thread safety: capture() may only be called from a single producer thread
 * (the gateway reactor thread).  writer_loop() is the single consumer.
 */
class FixCapture {
  public:
    enum class Direction : uint8_t {
        Inbound  = 0,
        Outbound = 1
    };

    /**
     * @param[in] file_path       Path to the capture output file.
     * @param[in] logger          Logger for error/warning messages.
     * @param[in] ring_bytes      Ring buffer capacity in bytes.  A larger
     *                            value gives more headroom if the writer thread
     *                            falls behind.  Typical value: 64 MB.
     */
    FixCapture(const std::string& file_path, pubsub_itc_fw::QuillLogger& logger,
               size_t ring_bytes);

    ~FixCapture();

    FixCapture(const FixCapture&) = delete;
    FixCapture& operator=(const FixCapture&) = delete;

    /**
     * @brief Enqueue a FIX wire record for capture.
     *
     * Called from the gateway thread.  Packs the record into the ring buffer
     * without any heap allocation.  If the ring is full the record is dropped
     * and a Warning is logged.
     */
    void capture(Direction direction, const uint8_t* data, size_t size, int64_t timestamp_ns);

    /**
     * @brief Returns the pthread_t of the writer thread for CPU-pinning
     *        registration.
     */
    [[nodiscard]] pthread_t writer_pthread_id() const { return writer_thread_.get_pthread_id(); }

  private:
    // Value written in the payload_size field of a sentinel record.
    // Must not clash with any real payload size; 0xFFFFFFFF is safe because
    // FIX messages are never 4 GB long.
    static constexpr uint32_t sentinel_size = 0xFFFFFFFFu;

    // Record header: payload_size(4) + timestamp_ns(8) + direction(1) = 13 bytes,
    // padded to 4-byte alignment = 16 bytes.
    static constexpr size_t header_bytes = 16;

    // Returns the total ring space consumed by a record with the given payload.
    static constexpr size_t slot_bytes(size_t payload) {
        return (header_bytes + payload + 3u) & ~3u;
    }

    void writer_loop();

    std::string                          file_path_;
    pubsub_itc_fw::QuillLogger&          logger_;
    std::vector<uint8_t>                 ring_;      // pre-allocated, never resized
    size_t                               capacity_;  // ring_.size()

    // Monotonically increasing byte offsets.  Actual position = offset % capacity_.
    // write_offset_ is written only by the gateway thread.
    // read_offset_  is written only by the writer thread.
    alignas(64) std::atomic<size_t> write_offset_{0};
    alignas(64) std::atomic<size_t> read_offset_{0};

    std::atomic<bool>                    shutdown_{false};
    pubsub_itc_fw::ThreadWithJoinTimeout writer_thread_;
};

} // namespace order_gateway
