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

void WalCursor::open(const std::string& directory, WalPosition from) {
    unmap_current();
    directory_ = directory;
    segment_numbers_.clear();

    DIR* dp = ::opendir(directory.c_str());
    if (dp != nullptr) {
        struct dirent* de = nullptr;
        while ((de = ::readdir(dp)) != nullptr) {
            uint64_t n = 0;
            if (std::sscanf(de->d_name, "wal_%06" SCNu64 ".log", &n) == 1) {
                segment_numbers_.push_back(n);
            }
        }
        ::closedir(dp);
        std::sort(segment_numbers_.begin(), segment_numbers_.end());
    } else if (errno != ENOENT) {
        throw PubSubItcException("WalCursor: opendir(" + directory + "): " + std::strerror(errno));
    }

    // Position at the first segment at or after from.segment.
    segment_index_ = 0;
    while (segment_index_ < segment_numbers_.size() && segment_numbers_[segment_index_] < from.segment) {
        ++segment_index_;
    }
    const bool on_from_segment = segment_index_ < segment_numbers_.size() && segment_numbers_[segment_index_] == from.segment;
    offset_ = on_from_segment ? static_cast<size_t>(from.offset) : 0;
    position_ = from;
}

bool WalCursor::read_next(int64_t& record_id, const uint8_t*& payload, size_t& size) {
    for (;;) {
        if (segment_index_ >= segment_numbers_.size()) {
            return false; // every segment exhausted
        }
        if (map_base_ == nullptr && !map_current_segment()) {
            // Segment missing or empty -- roll on to the next one.
            ++segment_index_;
            offset_ = 0;
            continue;
        }

        // End of this segment (no room for a header) -- roll on.
        if (offset_ + sizeof(WalEntryHeader) > map_size_) {
            unmap_current();
            ++segment_index_;
            offset_ = 0;
            continue;
        }

        const size_t entry_size = wal_entry_checks::valid_entry_size_at(map_base_, map_size_, offset_);
        if (entry_size == 0) {
            // Zeros to the end of the segment are space never written: this segment is finished.
            if (wal_entry_checks::all_zero(map_base_, offset_, map_size_)) {
                unmap_current();
                ++segment_index_;
                offset_ = 0;
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
                                                 segment_path(directory_, segment_numbers_[segment_index_]), offset_));
        }

        wal_entry_checks::EntryHeader header{};
        std::memcpy(&header, map_base_ + offset_, sizeof(header));
        record_id = header.record_id;
        payload = map_base_ + offset_ + sizeof(WalEntryHeader);
        size = static_cast<size_t>(header.payload_size);
        offset_ += entry_size;
        position_ = WalPosition{segment_numbers_[segment_index_], offset_};
        return true;
    }
}

bool WalCursor::later_segment_has_entries() const {
    for (size_t index = segment_index_ + 1; index < segment_numbers_.size(); ++index) {
        const std::string path = segment_path(directory_, segment_numbers_[index]);
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            continue;
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
    return false;
}

bool WalCursor::map_current_segment() {
    const std::string path = segment_path(directory_, segment_numbers_[segment_index_]);
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
    ::madvise(ptr, file_size, MADV_WILLNEED);

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
