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
     */
    [[nodiscard]] static WalPosition replay(const std::string& directory, WalPosition from, const EntryCallback& cb);

  private:
    // Scan one segment file starting at start_offset.
    // Returns the byte offset at which the scan stopped.
    static size_t replay_segment(const std::string& path, size_t start_offset, const EntryCallback& cb);
};

} // namespace pubsub_itc_fw
