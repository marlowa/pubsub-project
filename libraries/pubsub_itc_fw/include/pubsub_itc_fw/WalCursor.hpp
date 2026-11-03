#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

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
 * **Segments are found by their numbers.** The writer numbers segments one after another, and the log
 * only ever loses segments from its start or its end, so the segments on disk are numbered without
 * gaps. The cursor opens the segment it is positioned in by its number, and moves to the next by
 * opening the next number; it lists the directory only when the segment it is opened at does not
 * exist, to find the first segment after it. A day's WAL holds thousands of segments, and listing them
 * costs milliseconds, so a reader that opens a cursor at a known position must not pay for it.
 *
 * Not thread-safe. The payload pointer returned by read_next() is valid only until
 * the next read_next() call (or destruction), as it points into an mmap that a
 * segment roll-over may unmap.
 */
class WalCursor {
  public:
    /// How much of a segment the kernel is asked to read from the disk when the cursor maps it.
    enum class ReadAhead {
        /// The whole segment, for a reader that goes on through many records, such as one catching up.
        whole_segment,
        /// Only the pages the cursor reads, for a reader that reads a few records at a known position. Reading the
        /// whole 4 MiB segment from the disk to reach one record takes milliseconds.
        pages_read_only
    };

    ~WalCursor();

    WalCursor() = default;

    WalCursor(const WalCursor&) = delete;
    WalCursor& operator=(const WalCursor&) = delete;

    /**
     * @brief Open the WAL in `directory`, positioned to read entries at/after `from`.
     *
     * Re-opening resets all state. A directory that does not exist yet is valid: read_next() simply
     * returns false.
     *
     * @param[in] directory  The WAL's directory.
     * @param[in] from       Where to start reading. When its segment does not exist, reading starts at the
     *                       first segment after it.
     * @param[in] read_ahead How much of each segment to ask the kernel to read from the disk.
     */
    void open(const std::string& directory, WalPosition from, ReadAhead read_ahead = ReadAhead::whole_segment);

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
    // Whether a segment after the current one holds an entry, which means the writer has moved past the current one.
    [[nodiscard]] bool later_segment_has_entries() const;
    void unmap_current();
    // Moves to the next segment, or marks the cursor as having read every segment when there is none.
    void move_to_next_segment();
    [[nodiscard]] bool segment_exists(uint64_t segment) const;
    // The first segment numbered at or after @p segment, found by listing the directory, or nothing.
    [[nodiscard]] std::optional<uint64_t> first_segment_from(uint64_t segment) const;

    std::string directory_;
    ReadAhead read_ahead_{ReadAhead::whole_segment};
    uint64_t segment_{0};           // the number of the segment being read
    bool every_segment_read_{true}; // no segment remains to be read; stays so until the cursor is opened again
    size_t offset_{0};              // byte offset within the current segment
    WalPosition position_{};        // {segment number, offset} after the last read

    const uint8_t* map_base_{nullptr};
    size_t map_size_{0};
};

} // namespaces
