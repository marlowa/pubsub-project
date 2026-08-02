#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Resumable, one-record-at-a-time reader for a segmented WAL.
 *
 * Where WalReader::replay() pushes every entry through a callback in a single
 * call, WalCursor lets a caller *pull* one entry at a time and stop and resume at
 * will. This is what a streaming publisher needs: read the next record, send it,
 * wait for the socket to be writable, then read the next -- keeping the backlog in
 * the WAL (a position) rather than in memory.
 *
 * It mirrors WalReader's scan semantics exactly: entries are validated by magic
 * and CRC32; a zero-magic tail, a truncated entry, or a CRC mismatch ends the
 * current segment, and the cursor rolls on to the next segment (segments may have
 * unused zero-filled tails when an entry did not fit). read_next() returns false
 * only once every segment at/after the open position is exhausted.
 *
 * Not thread-safe. The payload pointer returned by read_next() is valid only until
 * the next read_next() call (or destruction), as it points into an mmap that a
 * segment roll-over may unmap.
 */
class WalCursor {
  public:
    ~WalCursor();

    WalCursor() = default;

    WalCursor(const WalCursor&) = delete;
    WalCursor& operator=(const WalCursor&) = delete;

    /**
     * @brief Open the WAL in `directory`, positioned to read entries at/after `from`.
     *
     * Discovers the segment files once. Re-opening resets all state. A directory
     * that does not exist yet is valid: read_next() simply returns false.
     */
    void open(const std::string& directory, WalPosition from);

    /**
     * @brief Read the next committed entry.
     *
     * @param[out] record_id  The record id stored at append time.
     * @param[out] payload     Pointer to the payload bytes (valid until the next call).
     * @param[out] size        Number of payload bytes.
     * @return true if an entry was read; false at end of committed data.
     */
    [[nodiscard]] bool read_next(int64_t& record_id, const uint8_t*& payload, size_t& size);

    /**
     * @brief Position immediately after the last entry returned by read_next().
     *
     * Equals the open position before the first read. Suitable for reopening later
     * to resume from the same point.
     */
    [[nodiscard]] WalPosition position() const {
        return position_;
    }

  private:
    bool map_current_segment();
    void unmap_current();

    std::string directory_;
    std::vector<uint64_t> segment_numbers_;
    size_t segment_index_{0}; // index into segment_numbers_
    size_t offset_{0};        // byte offset within the current segment
    WalPosition position_{};  // {segment number, offset} after the last read

    const uint8_t* map_base_{nullptr};
    size_t map_size_{0};
};

} // namespaces
