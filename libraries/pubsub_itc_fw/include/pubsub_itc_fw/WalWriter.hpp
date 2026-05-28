#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Single-writer mmap'd segmented write-ahead log.
 *
 * Stores opaque application payloads in a series of fixed-size, memory-mapped
 * segment files (`wal_NNNNNN.log`, zero-padded to 6 digits). Each entry is
 * framed with a standard header and a trailing CRC32:
 *
 *   [ WalEntryHeader (24 bytes) | payload (payload_size bytes) | CRC32 (4 bytes) ]
 *
 * Wire layout of WalEntryHeader (host byte order; endian conversion):
 *   magic(4) | payload_size(4) | record_id(8) | filler(8)
 *
 * The `record_id` is application-supplied and monotonically increasing. For the
 * sequencer it is the sequence number; for the arbiter it may be an epoch counter.
 * The framework uses it for replication acking without interpreting it.
 *
 * Durability:
 *   No fsync per commit. The OS writes dirty mmap pages to disk in the background,
 *   providing crash-survivability for process crashes (kernel survives) but not
 *   power loss. Replication is the durability guarantee; local disk is
 *   the single-host recovery mechanism.
 *
 * Threading: single-writer — all methods must be called from the same thread.
 *
 * Typical lifecycle:
 * @code
 *   WalPosition start = WalReader::replay(dir, {0,0}, cb); // returns end position
 *   WalWriter writer;
 *   writer.open(dir, segment_size, start);
 *   writer.append(seq_no, payload_ptr, payload_size);
 * @endcode
 */
class WalWriter {
  public:
    static constexpr uint32_t entry_magic = 0xFEEDFACEU;

    WalWriter() = default;
    ~WalWriter();

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    /**
     * @brief Opens the WAL directory and positions the writer at `start`.
     *
     * The directory is created if it does not exist. The segment file at
     * `start.segment` is opened (or created) and mapped. The writer will
     * append from `start.offset` within that segment.
     *
     * @param[in] directory    Directory for segment files.
     * @param[in] segment_size Pre-allocation size of each segment in bytes.
     * @param[in] start        Position at which to begin writing (from WalReader::replay()).
     * @throws std::runtime_error on any I/O failure.
     */
    void open(const std::string& directory, size_t segment_size, WalPosition start);

    /**
     * @brief Appends one record to the WAL (no fsync).
     *
     * Rolls to a new segment automatically when the current one is full.
     *
     * @param[in] record_id  Application-supplied monotonic record identifier.
     * @param[in] payload    Pointer to the payload bytes.
     * @param[in] size       Number of payload bytes.
     * @throws std::runtime_error if size exceeds segment_size.
     */
    void append(int64_t record_id, const void* payload, size_t size);

    /**
     * @brief Returns the current write position (segment + offset).
     *
     * Applications store this in their snapshot so that WalReader::replay()
     * can start from the anchor rather than from segment 0 on the next open.
     */
    [[nodiscard]] WalPosition current_position() const {
        return {current_segment_, write_offset_};
    }

    [[nodiscard]] bool is_open() const {
        return mmap_ptr_ != nullptr;
    }

  private:
    // Minimum space for any entry (header + 1 byte payload + CRC32).
    static constexpr size_t min_entry_bytes = 24 + 1 + sizeof(uint32_t);

    std::string segment_path(uint64_t seg_num) const;
    void open_segment(uint64_t seg_num);
    void close_segment();
    void ensure_capacity(size_t bytes_needed);

    std::string directory_;
    size_t segment_size_{0};
    uint64_t current_segment_{0};
    size_t write_offset_{0};

    uint8_t* mmap_ptr_{nullptr};
    int fd_{-1};
};

} // namespace pubsub_itc_fw
