// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalWriter.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/CpuPinning.hpp>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/FileSystemUtils.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

namespace {

// On-disk entry header -- internal to WalWriter/WalReader; not exposed to applications.
// Wire layout (host byte order; Slice 7 will add endian conversion):
//   magic(4) | payload_size(4) | record_id(8) | filler(8)  =  24 bytes
struct WalEntryHeader {
    uint32_t magic;
    uint32_t payload_size;
    int64_t record_id;
    uint64_t filler; // reserved, always zero
};
static_assert(sizeof(WalEntryHeader) == 24, "WalEntryHeader must be 24 bytes");

} // un-named namespace

WalWriter::~WalWriter() {
    stop_helper();
    release_all_slots();
    close_segment();
}

void WalWriter::close() {
    stop_helper();
    release_all_slots();
    close_segment();
}

void WalWriter::set_segments_kept_ready(size_t count) {
    if (count < 1) {
        throw PreconditionAssertion("WalWriter: segments_kept_ready must be at least 1", __FILE__, __LINE__);
    }
    segments_kept_ready_ = count;
}

// Path helper

std::string WalWriter::segment_path(uint64_t seg_num) const {
    return fmt::format("{}/wal_{:06}.log", directory_, seg_num);
}

std::string WalWriter::temporary_path(uint64_t seg_num, const char* ending) const {
    return fmt::format("{}/{}wal_{:06}.{}", directory_, temporary_prefix, seg_num, ending);
}

void WalWriter::warn(const std::string& message) const {
    if (warn_) {
        warn_(message);
    }
}

void WalWriter::open(const std::string& directory, size_t segment_size, WalPosition start) {
    if (segment_size < min_entry_bytes * 2) {
        throw PreconditionAssertion("WalWriter: segment_size too small", __FILE__, __LINE__);
    }

    directory_ = directory;
    segment_size_ = segment_size;

    const std::string mkdir_err = FileSystemUtils::make_directories(directory_);
    if (!mkdir_err.empty()) {
        throw PubSubItcException("WalWriter: " + mkdir_err);
    }

    // Before anything maps a segment: a temporary file is a fill cut short, and a short segment can
    // only have been left by a writer that filled segments under their final names. Mapped in full, a
    // short segment makes an append past its end fail with SIGBUS.
    remove_temporary_files();
    complete_short_segments(start.segment);

    current_segment_ = start.segment;
    write_offset_ = start.offset;

    // Nothing can prepare the first segment, so that one is always made or opened by the writer.
    ++segments_filled_inline_;
    adopt(create_and_map(current_segment_, writer_temporary_ending));

    slots_ = std::make_unique<Slot[]>(segments_kept_ready_);
    writing_.store(current_segment_, std::memory_order_release);
    prepared_through_.store(current_segment_, std::memory_order_release);
    helper_failed_.store(false, std::memory_order_release);
    if (preparation_enabled_) {
        start_helper();
    }
}

// create_and_map() -- the whole mechanism for one segment, with no thread in it.
//
// Called by the writer for the first segment and whenever the helper had none ready, and by the
// helper for every other segment. A file under the final name is always complete, because a new
// segment is filled under a temporary name and only then linked to its final name; link() fails if
// the name already exists, so if the writer and the helper both make the same segment, the first
// to link wins and the other opens that file.

WalWriter::OpenedSegment WalWriter::create_and_map(uint64_t seg_num, const char* temporary_ending) {
    const std::string path = segment_path(seg_num);
    OpenedSegment opened;
    opened.seg_num = seg_num;

    opened.fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC, 0644);
    if (opened.fd < 0) {
        if (errno != ENOENT) {
            throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
        }
        const std::string temporary = temporary_path(seg_num, temporary_ending);
        ::unlink(temporary.c_str());
        const int fd = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            throw PubSubItcException("WalWriter: open(" + temporary + "): " + std::strerror(errno));
        }
        fill_new_segment(fd, temporary);
        if (::link(temporary.c_str(), path.c_str()) == 0) {
            ::unlink(temporary.c_str());
            opened.fd = fd;
        } else if (errno == EEXIST) {
            // The other thread linked its copy first, and that one is complete.
            ::close(fd);
            ::unlink(temporary.c_str());
            opened.fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC, 0644);
            if (opened.fd < 0) {
                throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
            }
        } else {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            ::unlink(temporary.c_str());
            throw PubSubItcException("WalWriter: link(" + temporary + ", " + path + "): " + reason);
        }
    }

    void* ptr = ::mmap(nullptr, segment_size_, PROT_READ | PROT_WRITE, MAP_SHARED, opened.fd, 0);
    if (ptr == MAP_FAILED) {
        const std::string reason = std::strerror(errno);
        ::close(opened.fd);
        throw PubSubItcException("WalWriter: mmap(" + path + "): " + reason);
    }

    opened.mmap_ptr = static_cast<uint8_t*>(ptr);
    return opened;
}

void WalWriter::release(OpenedSegment& segment) {
    if (segment.mmap_ptr != nullptr) {
        ::munmap(segment.mmap_ptr, segment_size_);
    }
    if (segment.fd >= 0) {
        ::close(segment.fd);
    }
    segment = OpenedSegment{};
}

void WalWriter::adopt(const OpenedSegment& segment) {
    close_segment();
    fd_ = segment.fd;
    mmap_ptr_ = segment.mmap_ptr;
    current_segment_ = segment.seg_num;
}

void WalWriter::remove_temporary_files() {
    DIR* dir = ::opendir(directory_.c_str());
    if (dir == nullptr) {
        return;
    }
    std::vector<std::string> temporary;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.rfind(std::string(temporary_prefix) + "wal_", 0) == 0) {
            temporary.push_back(name);
        }
    }
    ::closedir(dir);
    for (const std::string& name : temporary) {
        ::unlink((directory_ + "/" + name).c_str());
    }
}

void WalWriter::complete_short_segments(uint64_t from) {
    DIR* dir = ::opendir(directory_.c_str());
    if (dir == nullptr) {
        return;
    }
    std::vector<uint64_t> segments;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.size() != 14 || name.compare(0, 4, "wal_") != 0 || name.compare(10, 4, ".log") != 0) {
            continue;
        }
        char* parsed_end = nullptr;
        const unsigned long long number = std::strtoull(name.c_str() + 4, &parsed_end, 10);
        if (parsed_end == name.c_str() + 10 && number >= from) {
            segments.push_back(number);
        }
    }
    ::closedir(dir);
    for (const uint64_t segment : segments) {
        const std::string path = segment_path(segment);
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0 || static_cast<size_t>(info.st_size) >= segment_size_) {
            continue;
        }
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
        }
        std::vector<uint8_t> zeros(fill_chunk_bytes, 0);
        size_t offset = static_cast<size_t>(info.st_size);
        while (offset < segment_size_) {
            const size_t chunk = std::min(segment_size_ - offset, zeros.size());
            const ssize_t written = ::pwrite(fd, zeros.data(), chunk, static_cast<off_t>(offset));
            if (written <= 0) {
                const std::string reason = std::strerror(errno);
                ::close(fd);
                throw PubSubItcException("WalWriter: completing " + path + ": " + reason);
            }
            offset += static_cast<size_t>(written);
        }
        ::close(fd);
        warn(fmt::format("WalWriter: {} was {} bytes, shorter than the segment size of {}: a process died while filling it. Completed with zeros, "
                         "keeping what it held",
                         path, info.st_size, segment_size_));
    }
}

void WalWriter::fill_new_segment(int fd, const std::string& path) {
    // Write the segment out in full rather than setting its size with ftruncate.
    //
    // ftruncate leaves the file SPARSE: the size is set and not one block is allocated. Every
    // page then gets its block on first write -- and since appending is a memcpy into a
    // mapping, that allocation happens inside a page fault on the thread doing the appending.
    // Allocating changes filesystem metadata, metadata changes need the journal, and a thread
    // waiting for a journal transaction to commit is in uninterruptible sleep: it cannot be
    // interrupted or preempted, and it is not on any run queue. Measured on the sequencer,
    // whose reactor thread appends every order the venue takes, those waits reached 557 ms.
    // See docs/bug_list.md BUG-0070.
    //
    // Writing the bytes allocates each block AND marks it written, so a later mapped write
    // touches no metadata at all. Measured over a 4 MiB segment, the first write to a page
    // goes from a p99 of about 40 microseconds to about 2.
    //
    // fallocate() is the obvious alternative and is NOT sufficient: on ext4 it reserves
    // extents in the UNWRITTEN state, and the first write to one must convert it to written,
    // which is itself a metadata change and journalled. Measured the same way, fallocate
    // improved the median and left the tail: a p99 of 42, 17 and 12 microseconds over three
    // rounds against 2, 4 and 1 for a segment written out in full. The tail is the whole
    // problem, so the median is not the thing to optimise.
    //
    // The cost is one sequential write per segment, which the page cache absorbs, against a
    // segment that then serves thousands of appends.
    std::vector<uint8_t> zeros(fill_chunk_bytes, 0);
    size_t remaining = segment_size_;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, zeros.size());
        const ssize_t written = ::write(fd, zeros.data(), chunk);
        if (written <= 0) {
            const std::string reason = std::strerror(errno);
            ::close(fd);
            throw PubSubItcException("WalWriter: writing " + path + ": " + reason);
        }
        remaining -= static_cast<size_t>(written);
    }
    if (::lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) {
        const std::string reason = std::strerror(errno);
        ::close(fd);
        throw PubSubItcException("WalWriter: lseek(" + path + "): " + reason);
    }
}

// The helper. It keeps up to segments_kept_ready segments ready ahead of the one being written, each in
// the slot for its number, and sleeps on an eventfd when it has nothing to do. The writer wakes it at
// each roll.

void WalWriter::start_helper() {
    helper_wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
    if (helper_wake_fd_ < 0) {
        // Not fatal. Without a helper every segment is made by the writer, which is slower at
        // each roll but correct, and segments_filled_inline() will show it.
        preparation_enabled_ = false;
        return;
    }
    stopping_.store(false, std::memory_order_release);
    helper_.start([this]() { helper_loop(); });
    helper_running_ = true;
}

void WalWriter::stop_helper() {
    if (!helper_running_) {
        if (helper_wake_fd_ >= 0) {
            ::close(helper_wake_fd_);
            helper_wake_fd_ = -1;
        }
        return;
    }

    stopping_.store(true, std::memory_order_release);
    wake_helper();

    // A generous timeout: the helper can be inside a write of one segment, and on a filesystem
    // that is committing a transaction that write is uninterruptible. Better to wait than to
    // detach a thread that is still writing into a file this object is about to unmap.
    (void)helper_.join_with_timeout(std::chrono::seconds{30});
    helper_running_ = false;

    ::close(helper_wake_fd_);
    helper_wake_fd_ = -1;
}

// wake_helper() -- one eventfd write.
//
// A failure here cannot be reported from where it happens and must not throw: the writer calls
// this at roll-over, on the order path. The consequence of a lost wake is that the helper sleeps
// until the next one and the writer may make a segment itself, which segments_filled_inline()
// counts and the warning function reports.

void WalWriter::wake_helper() {
    const uint64_t one = 1;
    if (::write(helper_wake_fd_, &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one))) {
        // A lost wake: see above. Nothing to do here.
        return;
    }
}

void WalWriter::helper_loop() {
    // Two things this thread must do for itself, before any work, because nothing else can do
    // them for it.
    //
    // It names itself. Linux gives a new thread the name of the thread that created it, so
    // without this the helper appears in every diagnostic as a second thread with the same name
    // as the component's application thread.
    //
    // It puts itself where it belongs. A new thread also inherits the processor mask of its
    // creator, and the log is opened by the application thread, by then pinned to a processor
    // reserved for the order path. Where no processors were named, it is left where it lands.
    pthread_setname_np(pthread_self(), "WalSegmentPrep");
    if (!helper_cores_.empty() && !apply_thread_affinity_mask(0, helper_cores_)) {
        // Not fatal, and not worth stopping a log for. The helper still works; it is merely in
        // the wrong place, which is what it was before anyone asked it to move.
        helper_placement_failed_ = true;
    }

    const uint64_t kept = segments_kept_ready_;
    while (!stopping_.load(std::memory_order_acquire)) {
        const uint64_t writing = writing_.load(std::memory_order_acquire);
        const uint64_t next = std::max(prepared_through_.load(std::memory_order_relaxed), writing) + 1;
        Slot& slot = slots_[next % kept];
        // Nothing to do while enough are ready, or while the slot still holds a segment the writer
        // has not yet freed; the writer wakes this thread at its next roll.
        if (next > writing + kept || slot.full.load(std::memory_order_acquire)) {
            uint64_t drained = 0;
            const ssize_t got = ::read(helper_wake_fd_, &drained, sizeof(drained));
            if (got != static_cast<ssize_t>(sizeof(drained)) && errno != EINTR) {
                return;
            }
            continue;
        }
        OpenedSegment made;
        if (helper_delay_for_tests_.count() > 0) {
            std::this_thread::sleep_for(helper_delay_for_tests_);
        }
        try {
            made = create_and_map(next, helper_temporary_ending);
        } catch (const std::exception& e) {
            // Never let an exception cross a thread boundary. The writer finds no segment ready,
            // makes it itself, says why, and throws there if it cannot.
            if (!helper_failed_.load(std::memory_order_acquire)) {
                helper_failure_ = e.what();
                helper_failed_.store(true, std::memory_order_release);
            }
            // Waits for the writer's next roll before trying again; a failed read is the same as a wake.
            uint64_t drained = 0;
            if (::read(helper_wake_fd_, &drained, sizeof(drained)) < 0) {
                continue;
            }
            continue;
        }
        // Published even if the writer has passed it meanwhile: a slot the writer has passed is freed
        // by the writer at its next roll, and only the writer frees a full slot.
        slot.segment = made;
        slot.full.store(true, std::memory_order_release);
        prepared_through_.store(next, std::memory_order_release);
    }
}

void WalWriter::release_passed_slots(uint64_t through) {
    for (size_t index = 0; index < segments_kept_ready_; ++index) {
        Slot& slot = slots_[index];
        if (slot.full.load(std::memory_order_acquire) && slot.segment.seg_num <= through) {
            release(slot.segment);
            slot.full.store(false, std::memory_order_release);
        }
    }
}

void WalWriter::release_all_slots() {
    if (slots_ == nullptr) {
        return;
    }
    for (size_t index = 0; index < segments_kept_ready_; ++index) {
        Slot& slot = slots_[index];
        if (slot.full.load(std::memory_order_acquire)) {
            release(slot.segment);
            slot.full.store(false, std::memory_order_release);
        }
    }
}

bool WalWriter::wait_until_prepared(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (prepared_through_.load(std::memory_order_acquire) >= writing_.load(std::memory_order_acquire) + segments_kept_ready_) {
            return true;
        }
        if (helper_failed_.load(std::memory_order_acquire)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return prepared_through_.load(std::memory_order_acquire) >= writing_.load(std::memory_order_acquire) + segments_kept_ready_;
}

void WalWriter::close_segment() {
    if (mmap_ptr_ != nullptr) {
        ::munmap(mmap_ptr_, segment_size_);
        mmap_ptr_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ensure_capacity() -- roll to next segment if needed

void WalWriter::ensure_capacity(size_t bytes_needed) {
    if (bytes_needed > segment_size_) {
        throw PreconditionAssertion("WalWriter: single entry exceeds segment_size", __FILE__, __LINE__);
    }
    if (write_offset_ + bytes_needed <= segment_size_) {
        return;
    }

    const uint64_t next = current_segment_ + 1;
    Slot* slot = slots_ == nullptr ? nullptr : &slots_[next % segments_kept_ready_];
    if (helper_running_ && slot != nullptr && slot->full.load(std::memory_order_acquire) && slot->segment.seg_num == next) {
        // The common path, and the point of the whole arrangement: no open, no fill, no mmap on
        // this thread, so nothing here can wait on the filesystem.
        adopt(slot->segment);
        slot->segment = OpenedSegment{};
        slot->full.store(false, std::memory_order_release);
    } else {
        // None ready. Make it here rather than wait for the helper, and never append into a segment
        // whose blocks do not exist -- that is the defect this exists to prevent. This is the one
        // wait for the disk an order-handling thread may make (principle F4), so it is said.
        const auto began = std::chrono::steady_clock::now();
        adopt(create_and_map(next, writer_temporary_ending));
        ++segments_filled_inline_;
        if (helper_running_) {
            const auto took = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
            const bool failed = helper_failed_.load(std::memory_order_acquire);
            warn(fmt::format("WalWriter: segment {} in {} was made by the writing thread, taking {} us, because none of the {} segment(s) kept ready "
                             "was ready{}",
                             next, directory_, took, segments_kept_ready_, failed ? " -- the helper failed: " + helper_failure_ : ""));
        }
    }

    // Any ready segment the writer has now passed, including one the helper finished just after the
    // writer made it here, is freed, so that its slot can take another.
    if (slots_ != nullptr) {
        release_passed_slots(next);
    }
    write_offset_ = 0;
    writing_.store(next, std::memory_order_release);
    if (helper_running_) {
        wake_helper();
    }
}

// append() -- the commit act

void WalWriter::append(int64_t record_id, const void* payload, size_t size) {
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
    writer_thread_.check();
#endif
    const size_t total = sizeof(WalEntryHeader) + size + sizeof(uint32_t);
    ensure_capacity(total);

    WalEntryHeader hdr{};
    hdr.magic = entry_magic;
    hdr.payload_size = static_cast<uint32_t>(size);
    hdr.record_id = record_id;
    hdr.filler = 0;

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

} // namespaces
