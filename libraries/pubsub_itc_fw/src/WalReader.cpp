// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalReader.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <vector>

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

using wal_entry_checks::all_zero;
using wal_entry_checks::next_valid_entry_after;
using wal_entry_checks::valid_entry_size_at;

namespace {

using WalEntryHeader = wal_entry_checks::EntryHeader;

std::string segment_path(const std::string& directory, uint64_t seg_num) {
    return fmt::format("{}/wal_{:06}.log", directory, seg_num);
}

} // un-named namespace

// scan_segment() -- read one segment file, and say how the reading stopped

WalReader::SegmentScan WalReader::scan_segment(const std::string& path, size_t start_offset, const EntryCallback& cb) {
    SegmentScan scan{start_offset, true};
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw PubSubItcException("WalReader: open(" + path + "): " + std::strerror(errno));
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw PubSubItcException("WalReader: fstat(" + path + "): " + std::strerror(errno));
    }
    const size_t file_size = static_cast<size_t>(st.st_size);

    if (file_size == 0 || start_offset >= file_size) {
        ::close(fd);
        return scan;
    }

    void* ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) {
        throw PubSubItcException("WalReader: mmap(" + path + "): " + std::strerror(errno));
    }

    ::madvise(ptr, file_size, MADV_WILLNEED);

    const auto* base = static_cast<const uint8_t*>(ptr);
    size_t offset = start_offset;

    for (;;) {
        const size_t entry_size = valid_entry_size_at(base, file_size, offset);
        if (entry_size != 0) {
            if (cb) {
                WalEntryHeader hdr{};
                std::memcpy(&hdr, base + offset, sizeof(WalEntryHeader));
                cb(hdr.record_id, base + offset + sizeof(WalEntryHeader), static_cast<size_t>(hdr.payload_size));
            }
            offset += entry_size;
            continue;
        }
        // The reading stops here. Zeros to the end of the segment are space never written: the end of
        // the data. Anything else is an entry that is damaged or not yet completely written.
        if (all_zero(base, offset, file_size)) {
            break;
        }
        const size_t later = next_valid_entry_after(base, file_size, offset);
        if (later == file_size) {
            // Nothing valid follows in this segment. Whether this is damage or the unfinished entry a
            // crash leaves, or one being written now, depends on whether anything valid follows in a
            // later segment, which replay() decides.
            scan.stopped_cleanly = false;
            break;
        }
        // A valid entry follows. If the one here is still not valid, the log is damaged. The writer
        // appends in order, so an entry being written when it was first looked at is complete by the
        // time a later one exists: look again before deciding.
        if (valid_entry_size_at(base, file_size, offset) != 0) {
            continue;
        }
        ::munmap(ptr, file_size);
        throw PubSubItcException(fmt::format("WalReader: the write-ahead log is damaged -- {} has an entry at byte {} that is not valid, followed by a "
                                             "valid entry at byte {}. Records after the damage would be lost, so the log is not read past it",
                                             path, offset, later));
    }

    ::munmap(ptr, file_size);
    scan.consumed = offset;
    return scan;
}

// replay_segment() -- scan one segment file; returns bytes consumed

size_t WalReader::replay_segment(const std::string& path, size_t start_offset, const EntryCallback& cb) {
    return scan_segment(path, start_offset, cb).consumed;
}

// replay() -- discover segments, replay from anchor, return end position

WalPosition WalReader::replay(const std::string& directory, WalPosition from, const EntryCallback& cb) {
    // Discover segment files in the directory.
    std::vector<uint64_t> seg_nums;
    {
        DIR* dp = ::opendir(directory.c_str());
        if (!dp) {
            if (errno == ENOENT) {
                // Directory doesn't exist yet -- nothing to replay.
                return from;
            }
            throw PubSubItcException("WalReader: opendir(" + directory + "): " + std::strerror(errno));
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
    // A segment whose reading stopped at bytes that were neither a valid entry nor unwritten space. If
    // a later segment holds a valid entry, those bytes are damage in the middle of the log, not the
    // unfinished entry at its end (BUG-0106).
    std::string unclean_path;
    size_t unclean_offset = 0;

    for (uint64_t seg : seg_nums) {
        if (seg < from.segment)
            continue; // fully covered by snapshot

        const size_t start = (seg == from.segment) ? static_cast<size_t>(from.offset) : 0;
        const std::string path = segment_path(directory, seg);
        if (!unclean_path.empty()) {
            // A writer that has moved on to this segment finished the entry it was writing in the last
            // one, so read that again from where it stopped before deciding.
            const SegmentScan again = scan_segment(unclean_path, unclean_offset, cb);
            if (again.consumed > unclean_offset) {
                end.segment = end.segment;
                end.offset = again.consumed;
            }
            if (!again.stopped_cleanly) {
                const SegmentScan here = scan_segment(path, start, nullptr);
                if (here.consumed > start) {
                    throw PubSubItcException(fmt::format("WalReader: the write-ahead log is damaged -- {} has an entry at byte {} that is not valid, and "
                                                         "{} holds valid entries after it. Records after the damage would be lost, so the log is not "
                                                         "read past it",
                                                         unclean_path, again.consumed, path));
                }
            }
            unclean_path.clear();
        }
        const SegmentScan scan = scan_segment(path, start, cb);
        const size_t consumed = scan.consumed;
        if (!scan.stopped_cleanly) {
            unclean_path = path;
            unclean_offset = consumed;
        }

        // Only move the end position for a segment that actually held something. A segment
        // created ahead of the writer is zero-filled, so it yields nothing and must not be
        // reported as where the log has reached: the answer would name a segment holding no
        // records, and the caller storing it in a snapshot would resume past the real tail.
        // The same happens after a crash, which leaves the prepared segment behind too.
        if (consumed > start) {
            end.segment = seg;
            end.offset = consumed;
        }
    }

    return end;
}

} // namespaces
