#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Sequential read-only scanner for a segmented WAL produced by WalWriter.
 *
 * Replays every valid entry from a starting position, invoking a callback for
 * each. An entry is valid if its magic matches and its CRC32 is correct.
 * The scan stops at the first zero-magic byte (end of committed data) or any
 * corrupted entry, treating everything beyond as "did not happen".
 *
 * Typical use (application startup):
 * @code
 *   WalPosition anchor = load_snapshot(); // {0,0} if no snapshot
 *   WalPosition end = WalReader::replay(directory, anchor,
 *       [](int64_t record_id, const void* payload, size_t size) {
 *           // rebuild application state from payload
 *       });
 *   writer.open(directory, segment_size, end);
 * @endcode
 */
class WalReader {
  public:
    /**
     * Callback invoked once per replayed entry.
     * @param record_id   Application record identifier stored at append time.
     * @param payload     Pointer to payload bytes (valid only for this call's duration).
     * @param size        Number of payload bytes.
     */
    using EntryCallback = std::function<void(int64_t record_id, const void* payload, size_t size)>;

    /**
     * @brief Replays all valid WAL entries from `from` onward.
     *
     * Scans segment files `wal_NNNNNN.log` in `directory` in ascending order,
     * starting at `from`. For each valid entry, `cb` is invoked. Scanning stops
     * at the end of committed data.
     *
     * @param[in] directory  Directory containing the segment files.
     * @param[in] from       Position to start scanning from (snapshot anchor or {0,0}).
     * @param[in] cb         Called for each valid entry (may be nullptr to skip callbacks).
     * @return The position immediately after the last committed entry. Pass this
     *         to WalWriter::open() to resume writing without gaps or overwrites.
     *
     * Reading stops at the first entry that is not valid. If that is the unfinished entry a crash
     * leaves, nothing valid follows it anywhere in the log, and replay returns normally. If a valid
     * entry follows it, in the same segment or a later one, the log is damaged in the middle, and
     * replay throws PubSubItcException naming the file and the byte, rather than carry on past the
     * damage and lose every record between it and the next segment (BUG-0106). An entry being written
     * while it is read is not mistaken for damage: the writer appends in order, so it is read again
     * once a later entry is found.
     */
    [[nodiscard]] static WalPosition replay(const std::string& directory, WalPosition from, const EntryCallback& cb);

    /**
     * @brief Replays the valid entries of one segment file, from `start_offset` to the end of its
     *        committed data.
     *
     * For a reader that needs one segment and not every later one, such as one that reads a log
     * backwards a segment at a time.
     *
     * @param[in] path          The segment file.
     * @param[in] start_offset  The byte offset of the first entry to read; zero for the whole segment.
     * @param[in] cb            Called for each valid entry (may be nullptr to skip callbacks).
     * @return The byte offset at which the scan stopped.
     */
    static size_t replay_segment(const std::string& path, size_t start_offset, const EntryCallback& cb);

  private:
    // How the reading of one segment ended.
    struct SegmentScan {
        // The byte offset at which the reading stopped.
        size_t consumed{0};
        // True when it stopped at space never written (zeros to the end of the segment), false when it
        // stopped at bytes that are neither a valid entry nor unwritten space.
        bool stopped_cleanly{true};
    };

    // Reads one segment, and throws PubSubItcException if an entry that is not valid is followed, in
    // the same segment, by one that is.
    static SegmentScan scan_segment(const std::string& path, size_t start_offset, const EntryCallback& cb);
};

} // namespaces
