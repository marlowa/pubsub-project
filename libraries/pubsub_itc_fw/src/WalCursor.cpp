// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalCursor.hpp>

#include <algorithm>
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

namespace pubsub_itc_fw {

namespace {

// On-disk entry framing, identical to WalReader/WalWriter: a 24-byte header, the
// payload, then a trailing CRC32 over header+payload.
struct WalEntryHeader {
    uint32_t magic;
    uint32_t payload_size;
    int64_t record_id;
    uint64_t filler;
};
static_assert(sizeof(WalEntryHeader) == 24, "WalEntryHeader must be 24 bytes");

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

        WalEntryHeader header{};
        std::memcpy(&header, map_base_ + offset_, sizeof(WalEntryHeader));

        // Zero-magic tail, truncated entry, or CRC mismatch all end this segment.
        const bool bad_magic = header.magic != WalWriter::entry_magic;
        const size_t entry_size = sizeof(WalEntryHeader) + header.payload_size + sizeof(uint32_t);
        const bool truncated = offset_ + entry_size > map_size_;
        if (bad_magic || truncated) {
            unmap_current();
            ++segment_index_;
            offset_ = 0;
            continue;
        }

        Crc32 crc;
        crc.feed(map_base_ + offset_, sizeof(WalEntryHeader) + header.payload_size);
        uint32_t stored = 0;
        std::memcpy(&stored, map_base_ + offset_ + sizeof(WalEntryHeader) + header.payload_size, sizeof(uint32_t));
        if (crc.finalize() != stored) {
            unmap_current();
            ++segment_index_;
            offset_ = 0;
            continue;
        }

        record_id = header.record_id;
        payload = map_base_ + offset_ + sizeof(WalEntryHeader);
        size = static_cast<size_t>(header.payload_size);
        offset_ += entry_size;
        position_ = WalPosition{segment_numbers_[segment_index_], offset_};
        return true;
    }
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
