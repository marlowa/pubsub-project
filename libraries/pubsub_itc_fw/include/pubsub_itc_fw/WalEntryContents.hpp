#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace pubsub_itc_fw {

/**
 * @brief What `Wal::append` stores in each entry of a write-ahead log (WAL), read back.
 *
 * `WalWriter` frames each entry with its own header, which holds the entry's number, and a checksum.
 * Inside that frame, `Wal::append` stores, in this order:
 *
 * | Field | Size | What it is |
 * |---|---|---|
 * | wall time | 8 bytes | When the leader numbered the record, in nanoseconds of the wall clock |
 * | leadership epoch | 4 bytes | The epoch of the leadership that numbered it, or zero where there is none |
 * | message type | 2 bytes | The type of the message in the bytes that follow |
 * | the record's bytes | the rest | The encoded message, exactly as the application gave it |
 *
 * The epoch is here, rather than inside the application's message, so that the library can tell which
 * leadership wrote each record without knowing anything about the application's messages: two WALs
 * that hold a record with the same number and the same epoch hold the same record, which is how a
 * rejoining follower's WAL is repaired (docs/availability/follower_log_repair.md).
 *
 * This is the one place that knows the layout. Code that reads entries with `WalReader` or `WalCursor`
 * calls read_wal_entry_contents() instead of counting out the header itself.
 */
struct WalEntryContents {
    /// When the leader numbered the record, in nanoseconds of the wall clock.
    int64_t wall_time_ns{0};
    /// The epoch of the leadership that numbered the record, or zero where there is none.
    int32_t leader_epoch{0};
    /// The type of the message in bytes.
    int16_t pdu_id{0};
    /// The record's bytes. Points into the entry that was read, so it is valid only as long as that is.
    const uint8_t* bytes{nullptr};
    /// How many bytes there are.
    size_t size{0};
};

/// The size of the fields `Wal::append` stores before the record's bytes.
inline constexpr size_t wal_entry_header_size = sizeof(int64_t) + sizeof(int32_t) + sizeof(int16_t);

/**
 * @brief Reads what an entry holds, or nothing if it is too short to hold the fields `Wal::append` stores.
 * @param[in] entry The entry's contents, as `WalReader` or `WalCursor` delivers them.
 * @param[in] size How many bytes the entry has.
 */
[[nodiscard]] inline std::optional<WalEntryContents> read_wal_entry_contents(const void* entry, size_t size) {
    if (size < wal_entry_header_size) {
        return std::nullopt;
    }
    const auto* data = static_cast<const uint8_t*>(entry);
    WalEntryContents contents;
    std::memcpy(&contents.wall_time_ns, data, sizeof(int64_t));
    std::memcpy(&contents.leader_epoch, data + sizeof(int64_t), sizeof(int32_t));
    std::memcpy(&contents.pdu_id, data + sizeof(int64_t) + sizeof(int32_t), sizeof(int16_t));
    contents.bytes = data + wal_entry_header_size;
    contents.size = size - wal_entry_header_size;
    return contents;
}

/**
 * @brief Writes the fields `Wal::append` stores before a record's bytes.
 * @param[out] out Where to write them: at least wal_entry_header_size bytes.
 * @param[in] wall_time_ns When the leader numbered the record, in nanoseconds of the wall clock.
 * @param[in] leader_epoch The epoch of the leadership that numbered the record, or zero where there is none.
 * @param[in] pdu_id The type of the message in the record's bytes.
 */
inline void write_wal_entry_header(uint8_t* out, int64_t wall_time_ns, int32_t leader_epoch, int16_t pdu_id) {
    std::memcpy(out, &wall_time_ns, sizeof(int64_t));
    std::memcpy(out + sizeof(int64_t), &leader_epoch, sizeof(int32_t));
    std::memcpy(out + sizeof(int64_t) + sizeof(int32_t), &pdu_id, sizeof(int16_t));
}

} // namespaces
