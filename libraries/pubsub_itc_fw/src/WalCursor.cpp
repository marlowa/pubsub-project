// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalCursor.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/WalEntryChecks.hpp>

namespace pubsub_itc_fw {

namespace {

// On-disk entry framing, identical to WalReader/WalWriter: a 24-byte header, the
// payload, then a trailing CRC32 over header+payload.
using WalEntryHeader = wal_entry_checks::EntryHeader;

std::string segment_path(const std::string& directory, uint64_t seg_num) {
    return fmt::format("{}/wal_{:06}.log", directory, seg_num);
}

} // un-named namespace

WalCursor::~WalCursor() {
    unmap_current();
}

void WalCursor::open(const std::string& directory, WalPosition from, ReadAhead read_ahead) {
    unmap_current();
    directory_ = directory;
    read_ahead_ = read_ahead;
    position_ = from;

    // The segment the position names, opened by its number, which is the case whenever the position came from
    // the log itself. Only when it does not exist is the directory listed, to find the first segment after it.
    if (segment_exists(from.segment)) {
        segment_ = from.segment;
        offset_ = static_cast<size_t>(from.offset);
        every_segment_read_ = false;
        return;
    }
    const std::optional<uint64_t> first = first_segment_from(from.segment);
    segment_ = first.value_or(0);
    offset_ = 0;
    every_segment_read_ = !first.has_value();
}

bool WalCursor::segment_exists(uint64_t segment) const {
    return ::access(segment_path(directory_, segment).c_str(), F_OK) == 0;
}

std::optional<uint64_t> WalCursor::first_segment_from(uint64_t segment) const {
    DIR* dp = ::opendir(directory_.c_str());
    if (dp == nullptr) {
        if (errno != ENOENT) {
            throw PubSubItcException("WalCursor: opendir(" + directory_ + "): " + std::strerror(errno));
        }
        return std::nullopt;
    }
    std::optional<uint64_t> first;
    struct dirent* de = nullptr;
    while ((de = ::readdir(dp)) != nullptr) {
        uint64_t n = 0;
        if (std::sscanf(de->d_name, "wal_%06" SCNu64 ".log", &n) == 1 && n >= segment && (!first.has_value() || n < *first)) {
            first = n;
        }
    }
    ::closedir(dp);
    return first;
}

void WalCursor::move_to_next_segment() {
    unmap_current();
    offset_ = 0;
    // The segments on disk are numbered without gaps, so when the next number does not exist, no later one does.
    // The cursor then stays finished, as a cursor that has read the last segment it found always has: a segment
    // the writer creates afterwards is read by a cursor opened again at position().
    if (segment_exists(segment_ + 1)) {
        ++segment_;
    } else {
        every_segment_read_ = true;
    }
}

bool WalCursor::read_next(int64_t& record_id, const uint8_t*& payload, size_t& size) {
    for (;;) {
        if (every_segment_read_) {
            return false;
        }
        if (map_base_ == nullptr && !map_current_segment()) {
            // Segment missing or empty -- roll on to the next one.
            move_to_next_segment();
            continue;
        }

        // End of this segment (no room for a header) -- roll on.
        if (offset_ + sizeof(WalEntryHeader) > map_size_) {
            move_to_next_segment();
            continue;
        }

        const size_t entry_size = wal_entry_checks::valid_entry_size_at(map_base_, map_size_, offset_);
        if (entry_size == 0) {
            // Zeros to the end of the segment are space never written: this segment is finished.
            if (wal_entry_checks::all_zero(map_base_, offset_, map_size_)) {
                move_to_next_segment();
                continue;
            }
            // Otherwise the entry here is damaged, or still being written. If nothing valid follows it,
            // in this segment or a later one, it is the end of the log for now: stay here, so that it is
            // read once it is complete. If something valid does follow, look again, because the writer
            // appends in order and an entry being written is complete once a later one exists; if it
            // is still not valid, the log is damaged, and reading past it would skip records (BUG-0106).
            const size_t later = wal_entry_checks::next_valid_entry_after(map_base_, map_size_, offset_);
            if (later == map_size_ && !later_segment_has_entries()) {
                return false;
            }
            if (wal_entry_checks::valid_entry_size_at(map_base_, map_size_, offset_) != 0) {
                continue;
            }
            throw PubSubItcException(fmt::format("WalCursor: the write-ahead log is damaged -- {} has an entry at byte {} that is not valid, followed by "
                                                 "valid entries. Records after the damage would be skipped, so the log is not read past it",
                                                 segment_path(directory_, segment_), offset_));
        }

        wal_entry_checks::EntryHeader header{};
        std::memcpy(&header, map_base_ + offset_, sizeof(header));
        record_id = header.record_id;
        payload = map_base_ + offset_ + sizeof(WalEntryHeader);
        size = static_cast<size_t>(header.payload_size);
        offset_ += entry_size;
        position_ = WalPosition{segment_, offset_};
        return true;
    }
}

bool WalCursor::later_segment_has_entries() const {
    // The later segments are the ones numbered after the current one, up to the first number that does not exist.
    for (uint64_t later = segment_ + 1;; ++later) {
        const std::string path = segment_path(directory_, later);
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        std::array<uint8_t, 4096> first{};
        const ssize_t got = ::pread(fd, first.data(), first.size(), 0);
        ::close(fd);
        // The first entry of a segment fits in its first page unless it is very large; a very large one
        // is checked by its magic number alone, which a segment never written does not have.
        if (got >= static_cast<ssize_t>(sizeof(wal_entry_checks::EntryHeader))) {
            if (wal_entry_checks::valid_entry_size_at(first.data(), static_cast<size_t>(got), 0) != 0) {
                return true;
            }
            uint32_t magic = 0;
            std::memcpy(&magic, first.data(), sizeof(magic));
            if (magic == WalWriter::entry_magic) {
                return true;
            }
        }
    }
}

bool WalCursor::map_current_segment() {
    const std::string path = segment_path(directory_, segment_);
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    const size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size == 0) {
        ::close(fd);
        return false;
    }

    void* ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) {
        throw PubSubItcException("WalCursor: mmap(" + path + "): " + std::strerror(errno));
    }
    // MADV_RANDOM stops the kernel reading the pages around each one the cursor touches, so a read of one record
    // from the disk reads the pages that hold it and no more.
    ::madvise(ptr, file_size, read_ahead_ == ReadAhead::whole_segment ? MADV_WILLNEED : MADV_RANDOM);

    map_base_ = static_cast<const uint8_t*>(ptr);
    map_size_ = file_size;
    return true;
}

void WalCursor::unmap_current() {
    if (map_base_ != nullptr) {
        ::munmap(const_cast<uint8_t*>(map_base_), map_size_);
        map_base_ = nullptr;
        map_size_ = 0;
    }
}

} // namespaces
