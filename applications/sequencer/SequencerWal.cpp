// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerWal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sequencer {

namespace {

// CRC32 using the IEEE polynomial (0xEDB88320, reflected).
// Table is computed once at startup via constexpr-friendly initialisation.

uint32_t crc32_table[256];

bool build_crc32_table() noexcept
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    return true;
}

// Initialised before main via static storage duration.
const bool crc32_table_ready = build_crc32_table();

struct Crc32Running {
    uint32_t state{0xFFFFFFFFu};

    void feed(const void* data, std::size_t len) noexcept
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i) {
            state = crc32_table[(state ^ p[i]) & 0xFFu] ^ (state >> 8);
        }
    }

    [[nodiscard]] uint32_t finalize() const noexcept { return state ^ 0xFFFFFFFFu; }
};

// Recursively create directories (like mkdir -p).
void make_directories(const std::string& path)
{
    // Walk the path component-by-component and mkdir each prefix.
    std::string cur;
    cur.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && i > 0) {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                throw std::runtime_error("SequencerWal: mkdir(" + cur + "): " + std::strerror(errno));
            }
        }
        cur += path[i];
    }
    // Final component.
    if (!cur.empty()) {
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            throw std::runtime_error("SequencerWal: mkdir(" + cur + "): " + std::strerror(errno));
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Destructor / lifecycle
// ---------------------------------------------------------------------------

SequencerWal::~SequencerWal()
{
    close_write_segment();
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string SequencerWal::segment_path(std::size_t seg_num) const
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/wal_%06zu.log", seg_num);
    return directory_ + buf;
}

std::string SequencerWal::snapshot_path() const
{
    return directory_ + "/snapshot.bin";
}

// ---------------------------------------------------------------------------
// open() — load snapshot, replay post-snapshot segments, position for writing
// ---------------------------------------------------------------------------

int64_t SequencerWal::open(const std::string& directory, std::size_t segment_size,
                           ReplayCallback replay_cb)
{
    if (segment_size < min_entry_bytes * 2) {
        throw std::runtime_error("SequencerWal: segment_size too small");
    }

    directory_    = directory;
    segment_size_ = segment_size;

    make_directories(directory_);

    // Load snapshot (if present). Sets last_seq_no_ and record_count_ from the
    // snapshot and returns the WAL anchor from which replay should begin.
    std::size_t replay_from_seg    = 0;
    std::size_t replay_from_offset = 0;
    load_snapshot(replay_from_seg, replay_from_offset);

    // Discover all existing segment files.
    std::vector<std::size_t> seg_nums;
    {
        DIR* dp = ::opendir(directory_.c_str());
        if (!dp) {
            throw std::runtime_error("SequencerWal: opendir(" + directory_ + "): " + std::strerror(errno));
        }
        struct dirent* de;
        while ((de = ::readdir(dp)) != nullptr) {
            std::size_t n = 0;
            if (std::sscanf(de->d_name, "wal_%06zu.log", &n) == 1) {
                seg_nums.push_back(n);
            }
        }
        ::closedir(dp);
        std::sort(seg_nums.begin(), seg_nums.end());
    }

    // Replay only segments at or after the snapshot anchor.
    // Segments before the anchor are fully covered by the snapshot state.
    std::vector<std::size_t> replay_segs;
    for (std::size_t s : seg_nums) {
        if (s >= replay_from_seg) replay_segs.push_back(s);
    }

    for (std::size_t i = 0; i < replay_segs.size(); ++i) {
        const std::size_t seg    = replay_segs[i];
        const std::size_t start  = (seg == replay_from_seg) ? replay_from_offset : 0;
        const std::size_t consumed = replay_segment(seg, start, replay_cb);
        if (i + 1 == replay_segs.size()) {
            current_segment_ = seg;
            write_offset_    = consumed;
        }
    }

    if (replay_segs.empty()) {
        // No post-snapshot segments on disk: resume at the snapshot anchor.
        current_segment_ = replay_from_seg;
        write_offset_    = replay_from_offset;
    }

    open_write_segment(current_segment_);

    return last_seq_no_;
}

// ---------------------------------------------------------------------------
// replay_segment() — read-only scan; returns bytes consumed
// ---------------------------------------------------------------------------

std::size_t SequencerWal::replay_segment(std::size_t seg_num, std::size_t start_offset,
                                         const ReplayCallback& replay_cb)
{
    const std::string path = segment_path(seg_num);

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("SequencerWal: open(" + path + "): " + std::strerror(errno));
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("SequencerWal: fstat(" + path + "): " + std::strerror(errno));
    }
    const std::size_t file_size = static_cast<std::size_t>(st.st_size);

    if (file_size == 0 || start_offset >= file_size) {
        ::close(fd);
        return start_offset;
    }

    void* ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (ptr == MAP_FAILED) {
        throw std::runtime_error("SequencerWal: mmap(" + path + "): " + std::strerror(errno));
    }

    ::madvise(ptr, file_size, MADV_WILLNEED);

    const auto* base         = static_cast<const uint8_t*>(ptr);
    std::size_t offset        = start_offset;
    std::size_t bytes_consumed = start_offset; // bytes before start_offset are already accounted for

    while (offset + sizeof(WalEntryHeader) <= file_size) {
        WalEntryHeader hdr;
        std::memcpy(&hdr, base + offset, sizeof(WalEntryHeader));

        if (hdr.magic != entry_magic) {
            // End of committed data.
            break;
        }

        const std::size_t entry_size =
            sizeof(WalEntryHeader) + hdr.payload_size + sizeof(uint32_t);

        if (offset + entry_size > file_size) {
            // Truncated entry — treat as uncommitted tail.
            break;
        }

        // Verify CRC32 (covers header + payload).
        Crc32Running crc;
        crc.feed(base + offset, sizeof(WalEntryHeader) + hdr.payload_size);
        const uint32_t computed = crc.finalize();

        uint32_t stored;
        std::memcpy(&stored, base + offset + sizeof(WalEntryHeader) + hdr.payload_size, sizeof(uint32_t));

        if (computed != stored) {
            // Corrupted entry — stop replay here.
            break;
        }

        ++record_count_;
        last_seq_no_ = hdr.seq_no;

        if (replay_cb) {
            replay_cb(hdr.seq_no, hdr.pdu_id,
                      base + offset + sizeof(WalEntryHeader),
                      static_cast<std::size_t>(hdr.payload_size));
        }

        offset += entry_size;
        bytes_consumed = offset;
    }

    ::munmap(ptr, file_size);
    return bytes_consumed;
}

// ---------------------------------------------------------------------------
// load_snapshot() — read and validate snapshot.bin
// ---------------------------------------------------------------------------

bool SequencerWal::load_snapshot(std::size_t& out_seg, std::size_t& out_offset)
{
    const std::string path = snapshot_path();

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("SequencerWal: load_snapshot open(" + path + "): " + std::strerror(errno));
    }

    SnapshotHeader hdr{};
    const ssize_t  n = ::read(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        // Truncated or empty — treat as absent (fall back to full replay).
        return false;
    }

    if (hdr.magic != snapshot_magic || hdr.version != snapshot_version) {
        return false;
    }

    Crc32Running crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    if (crc.finalize() != hdr.checksum) {
        return false;
    }

    last_seq_no_  = hdr.last_seq_no;
    record_count_ = hdr.record_count;
    out_seg       = static_cast<std::size_t>(hdr.wal_segment);
    out_offset    = static_cast<std::size_t>(hdr.wal_offset);
    return true;
}

// ---------------------------------------------------------------------------
// take_snapshot() — checkpoint state, then delete superseded WAL segments
// ---------------------------------------------------------------------------

void SequencerWal::take_snapshot()
{
    SnapshotHeader hdr{};
    hdr.magic        = snapshot_magic;
    hdr.version      = snapshot_version;
    hdr.last_seq_no  = last_seq_no_;
    hdr.record_count = static_cast<uint64_t>(record_count_);
    hdr.wal_segment  = static_cast<uint64_t>(current_segment_);
    hdr.wal_offset   = static_cast<uint64_t>(write_offset_);
    hdr.filler       = 0;

    Crc32Running crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    hdr.checksum = crc.finalize();

    const std::string tmp   = snapshot_path() + ".tmp";
    const std::string final = snapshot_path();

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("SequencerWal: take_snapshot open(" + tmp + "): " + std::strerror(errno));
    }

    const ssize_t written = ::write(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (written != static_cast<ssize_t>(sizeof(hdr))) {
        throw std::runtime_error("SequencerWal: take_snapshot write to " + tmp + " incomplete");
    }

    if (::rename(tmp.c_str(), final.c_str()) != 0) {
        throw std::runtime_error("SequencerWal: take_snapshot rename(" + tmp + " -> " + final + "): " + std::strerror(errno));
    }

    // Snapshot is committed. Delete segments fully covered by it.
    delete_segments_before(current_segment_);
}

// ---------------------------------------------------------------------------
// delete_segments_before() — remove old WAL segments (best-effort)
// ---------------------------------------------------------------------------

void SequencerWal::delete_segments_before(std::size_t seg_num) noexcept
{
    for (std::size_t i = 0; i < seg_num; ++i) {
        ::unlink(segment_path(i).c_str()); // ignore ENOENT and all other errors
    }
}

// ---------------------------------------------------------------------------
// open_write_segment() — open or create a segment for writing
// ---------------------------------------------------------------------------

void SequencerWal::open_write_segment(std::size_t seg_num)
{
    close_write_segment();

    const std::string path = segment_path(seg_num);

    // Try O_EXCL first: succeeds only for a brand-new file.
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd_ >= 0) {
        // New file: pre-allocate to segment_size (all zeros = sentinel for replay).
        if (::ftruncate(fd_, static_cast<off_t>(segment_size_)) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("SequencerWal: ftruncate(" + path + "): " + std::strerror(errno));
        }
    } else if (errno == EEXIST) {
        // Existing segment (resumed after replay): open without truncating.
        fd_ = ::open(path.c_str(), O_RDWR, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("SequencerWal: open(" + path + "): " + std::strerror(errno));
        }
    } else {
        throw std::runtime_error("SequencerWal: open(" + path + "): " + std::strerror(errno));
    }

    void* ptr = ::mmap(nullptr, segment_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("SequencerWal: mmap(" + path + "): " + std::strerror(errno));
    }

    mmap_ptr_        = static_cast<uint8_t*>(ptr);
    current_segment_ = seg_num;
}

// ---------------------------------------------------------------------------
// close_write_segment()
// ---------------------------------------------------------------------------

void SequencerWal::close_write_segment() noexcept
{
    if (mmap_ptr_ != nullptr) {
        ::munmap(mmap_ptr_, segment_size_);
        mmap_ptr_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// ensure_capacity() — roll to next segment if needed
// ---------------------------------------------------------------------------

void SequencerWal::ensure_capacity(std::size_t bytes_needed)
{
    if (bytes_needed > segment_size_) {
        throw std::runtime_error("SequencerWal: single entry exceeds segment_size");
    }
    if (write_offset_ + bytes_needed > segment_size_) {
        open_write_segment(current_segment_ + 1);
        write_offset_ = 0;
    }
}

// ---------------------------------------------------------------------------
// append() — the commit act
// ---------------------------------------------------------------------------

void SequencerWal::append(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size)
{
    const auto payload_size  = static_cast<uint32_t>(size);
    const std::size_t total  = sizeof(WalEntryHeader) + payload_size + sizeof(uint32_t);

    ensure_capacity(total);

    WalEntryHeader hdr{};
    hdr.magic        = entry_magic;
    hdr.payload_size = payload_size;
    hdr.seq_no       = seq_no;
    hdr.pdu_id       = pdu_id;
    hdr.filler_a     = 0;
    hdr.filler_b     = 0;

    uint8_t* dest = mmap_ptr_ + write_offset_;

    std::memcpy(dest, &hdr, sizeof(WalEntryHeader));
    std::memcpy(dest + sizeof(WalEntryHeader), payload, payload_size);

    Crc32Running crc;
    crc.feed(&hdr, sizeof(WalEntryHeader));
    crc.feed(payload, payload_size);
    const uint32_t checksum = crc.finalize();

    std::memcpy(dest + sizeof(WalEntryHeader) + payload_size, &checksum, sizeof(uint32_t));

    write_offset_ += total;
    last_seq_no_   = seq_no;
    ++record_count_;
}

} // namespace sequencer
