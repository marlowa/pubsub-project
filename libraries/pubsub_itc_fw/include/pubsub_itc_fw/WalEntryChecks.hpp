#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

// The checks both readers of a write-ahead log segment make, WalReader and WalCursor, to tell an entry
// that is valid from one that is damaged or not yet completely written, and to tell either from space
// never written (docs/bug_list.md, BUG-0106). For the framework's own readers of the log, not for applications.
namespace pubsub_itc_fw::wal_entry_checks {

/// The header at the start of every entry, as WalWriter writes it.
struct EntryHeader {
    uint32_t magic;
    uint32_t payload_size;
    int64_t record_id;
    uint64_t filler;
};
static_assert(sizeof(EntryHeader) == 24, "EntryHeader must be 24 bytes");

/// The length of a valid entry starting at @p offset, or zero if the bytes there are not one: the magic
/// number, a length that fits in the segment, and a checksum that matches.
inline size_t valid_entry_size_at(const uint8_t* base, size_t segment_size, size_t offset) {
    if (offset + sizeof(EntryHeader) + sizeof(uint32_t) > segment_size) {
        return 0;
    }
    EntryHeader header{};
    std::memcpy(&header, base + offset, sizeof(EntryHeader));
    if (header.magic != WalWriter::entry_magic) {
        return 0;
    }
    const size_t entry_size = sizeof(EntryHeader) + header.payload_size + sizeof(uint32_t);
    if (header.payload_size > segment_size || offset + entry_size > segment_size) {
        return 0;
    }
    Crc32 crc;
    crc.feed(base + offset, sizeof(EntryHeader) + header.payload_size);
    uint32_t stored{};
    std::memcpy(&stored, base + offset + sizeof(EntryHeader) + header.payload_size, sizeof(uint32_t));
    return crc.finalize() == stored ? entry_size : 0;
}

/// The offset of the first valid entry after @p from, or @p segment_size if there is none. Entries are
/// not aligned, so every byte position holding the magic number is tried.
inline size_t next_valid_entry_after(const uint8_t* base, size_t segment_size, size_t from) {
    const uint32_t magic = WalWriter::entry_magic;
    for (size_t offset = from + 1; offset + sizeof(EntryHeader) <= segment_size; ++offset) {
        if (std::memcmp(base + offset, &magic, sizeof(magic)) == 0 && valid_entry_size_at(base, segment_size, offset) != 0) {
            return offset;
        }
    }
    return segment_size;
}

/// Whether every byte from @p from up to @p to is zero: space never written.
inline bool all_zero(const uint8_t* base, size_t from, size_t to) {
    for (size_t offset = from; offset < to; ++offset) {
        if (base[offset] != 0) {
            return false;
        }
    }
    return true;
}

} // namespaces
