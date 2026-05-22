// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalReader.hpp>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

namespace pubsub_itc_fw {

namespace {

struct WalEntryHeader {
    uint32_t magic;
    uint32_t payload_size;
    int64_t  record_id;
    uint64_t filler;
};
static_assert(sizeof(WalEntryHeader) == 24, "WalEntryHeader must be 24 bytes");

std::string segment_path(const std::string& directory, uint64_t seg_num)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/wal_%06" PRIu64 ".log", seg_num);
    return directory + buf;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// replay_segment() — scan one segment file; returns bytes consumed
// ---------------------------------------------------------------------------

size_t WalReader::replay_segment(const std::string& path, size_t start_offset,
                                      const EntryCallback& cb)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("WalReader: open(" + path + "): " + std::strerror(errno));
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("WalReader: fstat(" + path + "): " + std::strerror(errno));
    }
    const size_t file_size = static_cast<size_t>(st.st_size);

    if (file_size == 0 || start_offset >= file_size) {
        ::close(fd);
        return start_offset;
    }

    void* ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) {
        throw std::runtime_error("WalReader: mmap(" + path + "): " + std::strerror(errno));
    }

    ::madvise(ptr, file_size, MADV_WILLNEED);

    const auto* base           = static_cast<const uint8_t*>(ptr);
    size_t offset         = start_offset;
    size_t bytes_consumed = start_offset;

    while (offset + sizeof(WalEntryHeader) <= file_size) {
        WalEntryHeader hdr{};
        std::memcpy(&hdr, base + offset, sizeof(WalEntryHeader));

        if (hdr.magic != WalWriter::entry_magic) {
            break; // end of committed data
        }

        const size_t entry_size = sizeof(WalEntryHeader) + hdr.payload_size + sizeof(uint32_t);
        if (offset + entry_size > file_size) {
            break; // truncated entry — treat as uncommitted tail
        }

        Crc32 crc;
        crc.feed(base + offset, sizeof(WalEntryHeader) + hdr.payload_size);
        const uint32_t computed = crc.finalize();

        uint32_t stored{};
        std::memcpy(&stored, base + offset + sizeof(WalEntryHeader) + hdr.payload_size, sizeof(uint32_t));

        if (computed != stored) {
            break; // corrupted entry — stop replay
        }

        if (cb) {
            cb(hdr.record_id,
               base + offset + sizeof(WalEntryHeader),
               static_cast<size_t>(hdr.payload_size));
        }

        offset += entry_size;
        bytes_consumed = offset;
    }

    ::munmap(ptr, file_size);
    return bytes_consumed;
}

// ---------------------------------------------------------------------------
// replay() — discover segments, replay from anchor, return end position
// ---------------------------------------------------------------------------

WalPosition WalReader::replay(const std::string& directory, WalPosition from,
                              const EntryCallback& cb)
{
    // Discover segment files in the directory.
    std::vector<uint64_t> seg_nums;
    {
        DIR* dp = ::opendir(directory.c_str());
        if (!dp) {
            if (errno == ENOENT) {
                // Directory doesn't exist yet — nothing to replay.
                return from;
            }
            throw std::runtime_error("WalReader: opendir(" + directory + "): " + std::strerror(errno));
        }
        struct dirent* de;
        while ((de = ::readdir(dp)) != nullptr) {
            uint64_t n = 0;
            if (std::sscanf(de->d_name, "wal_%06" SCNu64 ".log", &n) == 1) {
                seg_nums.push_back(n);
            }
        }
        ::closedir(dp);
        std::sort(seg_nums.begin(), seg_nums.end());
    }

    WalPosition end = from;

    for (uint64_t seg : seg_nums) {
        if (seg < from.segment) continue; // fully covered by snapshot

        const size_t start = (seg == from.segment) ? static_cast<size_t>(from.offset) : 0;
        const std::string path  = segment_path(directory, seg);
        const size_t consumed = replay_segment(path, start, cb);

        end.segment = seg;
        end.offset  = consumed;
    }

    return end;
}

} // namespace pubsub_itc_fw
