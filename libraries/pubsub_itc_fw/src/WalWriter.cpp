// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalWriter.hpp>

#include <cerrno>
#include <cinttypes>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/utils/FileSystemUtils.hpp>

namespace pubsub_itc_fw {

namespace {

// On-disk entry header — internal to WalWriter/WalReader; not exposed to applications.
// Wire layout (host byte order; Slice 7 will add endian conversion):
//   magic(4) | payload_size(4) | record_id(8) | filler(8)  =  24 bytes
struct WalEntryHeader {
    uint32_t magic;
    uint32_t payload_size;
    int64_t  record_id;
    uint64_t filler; // reserved, always zero
};
static_assert(sizeof(WalEntryHeader) == 24, "WalEntryHeader must be 24 bytes");

} // anonymous namespace

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

WalWriter::~WalWriter()
{
    close_segment();
}

// ---------------------------------------------------------------------------
// Path helper
// ---------------------------------------------------------------------------

std::string WalWriter::segment_path(uint64_t seg_num) const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/wal_%06" PRIu64 ".log", seg_num);
    return directory_ + buf;
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------

void WalWriter::open(const std::string& directory, size_t segment_size, WalPosition start)
{
    if (segment_size < min_entry_bytes * 2) {
        throw PreconditionAssertion("WalWriter: segment_size too small", __FILE__, __LINE__);
    }

    directory_    = directory;
    segment_size_ = segment_size;

    const std::string mkdir_err = FileSystemUtils::make_directories(directory_);
    if (!mkdir_err.empty()) {
        throw PubSubItcException("WalWriter: " + mkdir_err);
    }

    current_segment_ = start.segment;
    write_offset_    = start.offset;

    open_segment(current_segment_);
}

// ---------------------------------------------------------------------------
// open_segment() — open or create a segment for writing
// ---------------------------------------------------------------------------

void WalWriter::open_segment(uint64_t seg_num)
{
    close_segment();

    const std::string path = segment_path(seg_num);

    // Try exclusive create first: succeeds only for a brand-new file.
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd_ >= 0) {
        if (::ftruncate(fd_, static_cast<off_t>(segment_size_)) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw PubSubItcException("WalWriter: ftruncate(" + path + "): " + std::strerror(errno));
        }
    } else if (errno == EEXIST) {
        // Resuming after replay: open without truncating.
        fd_ = ::open(path.c_str(), O_RDWR, 0644);
        if (fd_ < 0) {
            throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
        }
    } else {
        throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
    }

    void* ptr = ::mmap(nullptr, segment_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        throw PubSubItcException("WalWriter: mmap(" + path + "): " + std::strerror(errno));
    }

    mmap_ptr_        = static_cast<uint8_t*>(ptr);
    current_segment_ = seg_num;
}

// ---------------------------------------------------------------------------
// close_segment()
// ---------------------------------------------------------------------------

void WalWriter::close_segment() noexcept
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

void WalWriter::ensure_capacity(size_t bytes_needed)
{
    if (bytes_needed > segment_size_) {
        throw PreconditionAssertion("WalWriter: single entry exceeds segment_size", __FILE__, __LINE__);
    }
    if (write_offset_ + bytes_needed > segment_size_) {
        open_segment(current_segment_ + 1);
        write_offset_ = 0;
    }
}

// ---------------------------------------------------------------------------
// append() — the commit act
// ---------------------------------------------------------------------------

void WalWriter::append(int64_t record_id, const void* payload, size_t size)
{
    const size_t total = sizeof(WalEntryHeader) + size + sizeof(uint32_t);
    ensure_capacity(total);

    WalEntryHeader hdr{};
    hdr.magic        = entry_magic;
    hdr.payload_size = static_cast<uint32_t>(size);
    hdr.record_id    = record_id;
    hdr.filler       = 0;

    uint8_t* dest = mmap_ptr_ + write_offset_;

    std::memcpy(dest, &hdr, sizeof(WalEntryHeader));
    std::memcpy(dest + sizeof(WalEntryHeader), payload, size);

    Crc32 crc;
    crc.feed(&hdr, sizeof(WalEntryHeader));
    crc.feed(payload, size);
    const uint32_t checksum = crc.finalize();

    std::memcpy(dest + sizeof(WalEntryHeader) + size, &checksum, sizeof(uint32_t));

    write_offset_ += total;
}

} // namespace pubsub_itc_fw
