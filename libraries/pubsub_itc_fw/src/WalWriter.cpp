// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalWriter.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>

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
    discard_prepared();
    close_segment();
}

// Path helper

std::string WalWriter::segment_path(uint64_t seg_num) const {
    return fmt::format("{}/wal_{:06}.log", directory_, seg_num);
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

    current_segment_ = start.segment;
    write_offset_ = start.offset;

    open_segment(current_segment_);

    // Nothing can prepare the first segment, so that one is always made by the writer. From
    // here on the helper stays a segment ahead, and has the whole life of the current segment
    // to do it in.
    if (preparation_enabled_) {
        start_helper();
        request_preparation(current_segment_ + 1);
    }
}

// open_segment() -- open or create a segment for writing

// create_and_map() -- the whole mechanism, with no thread in it.
//
// Called by the writer for the first segment and whenever the helper was not ready, and by the
// helper for every other segment. It touches nothing the other thread can see: a segment number
// in, a descriptor and a mapping out.

WalWriter::OpenedSegment WalWriter::create_and_map(uint64_t seg_num) {
    const std::string path = segment_path(seg_num);
    OpenedSegment opened;
    opened.seg_num = seg_num;

    // Try exclusive create first: succeeds only for a brand-new file.
    opened.fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if (opened.fd >= 0) {
        fill_new_segment(opened.fd, path);
    } else if (errno == EEXIST) {
        // Resuming after replay, or adopting a segment prepared before the last shutdown.
        opened.fd = ::open(path.c_str(), O_RDWR, 0644);
        if (opened.fd < 0) {
            throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
        }
    } else {
        throw PubSubItcException("WalWriter: open(" + path + "): " + std::strerror(errno));
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

void WalWriter::adopt(const OpenedSegment& segment) {
    close_segment();
    fd_ = segment.fd;
    mmap_ptr_ = segment.mmap_ptr;
    current_segment_ = segment.seg_num;
}

void WalWriter::open_segment(uint64_t seg_num) {
    ++segments_filled_inline_;
    adopt(create_and_map(seg_num));
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

// The helper waits on an eventfd, creates one segment when asked, and publishes it. The writer
// reads what it published only after seeing Ready, and writes requested_segment_ only while the
// state is Idle or Failed, so the two never touch the same field at the same time.

void WalWriter::start_helper() {
    helper_wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
    if (helper_wake_fd_ < 0) {
        // Not fatal. Without a helper every segment is made by the writer, which is slower at
        // each roll but correct, and segments_filled_inline() will show it.
        preparation_enabled_ = false;
        return;
    }
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

    prep_state_.store(PrepState::Stopping, std::memory_order_release);
    wake_helper();

    // A generous timeout: the helper can be inside a write of one segment, and on a filesystem
    // that is committing a transaction that write is uninterruptible. Better to wait than to
    // detach a thread that is still writing into a file this object is about to unmap.
    (void)helper_.join_with_timeout(std::chrono::seconds{30});
    helper_running_ = false;

    ::close(helper_wake_fd_);
    helper_wake_fd_ = -1;
}

void WalWriter::request_preparation(uint64_t seg_num) {
    if (!helper_running_) {
        return;
    }

    // Only ever one request outstanding, and only issued from a state in which the helper is
    // not looking at requested_segment_. Writing it while the helper was reading it was a real
    // race, found by ThreadSanitizer and by nothing else: the functional tests passed.
    const PrepState state = prep_state_.load(std::memory_order_acquire);
    if (state != PrepState::Idle && state != PrepState::Failed) {
        return;
    }

    requested_segment_ = seg_num;
    prep_state_.store(PrepState::Requested, std::memory_order_release);
    wake_helper();
}

// await_helper() -- waits for a preparation already in progress to finish.
//
// Reached only when the helper has not kept pace, which in steady state does not happen: it has
// the whole life of a segment to prepare the next one. Waiting is not a cost this adds. If the
// writer gave up and made the segment itself it would pay the same block allocation, and it
// would be creating the very file the helper is still writing -- which mmaps a partly written
// file and makes an ordinary append a SIGBUS. Waiting is therefore both safer and no slower.

void WalWriter::await_helper() {
    ++segments_waited_for_;
    while (prep_state_.load(std::memory_order_acquire) == PrepState::Requested) {
        std::this_thread::yield();
    }
}

// wake_helper() -- one eventfd write.
//
// A failure here cannot be reported from where it happens and must not throw: the writer calls
// this at roll-over, on the order path. The consequence of a lost wake is that the helper stays
// asleep and the writer makes the next segment itself, which segments_filled_inline() counts.

void WalWriter::wake_helper() {
    const uint64_t one = 1;
    const ssize_t written = ::write(helper_wake_fd_, &one, sizeof(one));
    if (written != static_cast<ssize_t>(sizeof(one))) {
        prep_state_.store(PrepState::Failed, std::memory_order_release);
    }
}

void WalWriter::helper_loop() {
    while (true) {
        uint64_t drained = 0;
        const ssize_t got = ::read(helper_wake_fd_, &drained, sizeof(drained));
        if (got != static_cast<ssize_t>(sizeof(drained))) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        const PrepState state = prep_state_.load(std::memory_order_acquire);
        if (state == PrepState::Stopping) {
            return;
        }
        if (state != PrepState::Requested) {
            continue;
        }

        // requested_segment_ was written before the release-store of Requested that this thread
        // has just acquired, so it is visible and will not change until the state leaves Ready.
        const uint64_t wanted = requested_segment_;
        PrepState outcome = PrepState::Ready;
        try {
            prepared_ = create_and_map(wanted);
        } catch (const std::exception& e) {
            // Never let an exception cross a thread boundary. The writer finds Failed at the
            // next roll, makes the segment itself, and throws there if it cannot -- so the
            // failure is reported on the thread that can do something about it.
            failure_ = e.what();
            outcome = PrepState::Failed;
        }

        // Publish only if nobody asked us to stop while we were working. A plain store here
        // would overwrite Stopping, and the next read() would then block forever with no wake
        // left to come: the destructor would wait out its whole join timeout on every close.
        PrepState expected = PrepState::Requested;
        if (!prep_state_.compare_exchange_strong(expected, outcome, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (prepared_.mmap_ptr != nullptr) {
                ::munmap(prepared_.mmap_ptr, segment_size_);
            }
            if (prepared_.fd >= 0) {
                ::close(prepared_.fd);
            }
            prepared_ = OpenedSegment{};
            return;
        }
    }
}

void WalWriter::release_prepared_if_ready() {
    // Only safe while the state is Ready: that is the one state in which the helper has
    // finished with prepared_ and is waiting, so this thread may touch it. In any other state
    // the helper may be writing those fields, and the segment it eventually publishes is
    // released at a later roll, or by discard_prepared() at close.
    if (prep_state_.load(std::memory_order_acquire) != PrepState::Ready) {
        return;
    }
    discard_prepared();
    prep_state_.store(PrepState::Idle, std::memory_order_release);
}

void WalWriter::discard_prepared() {
    // Frees whatever prepared_ holds, whatever the state says. Called at close, after the
    // helper has been joined, so there is no other thread to disagree with -- and it must not
    // consult the state, because stop_helper() leaves it at Stopping while a segment the helper
    // published just beforehand is still sitting in prepared_ with a descriptor and a mapping.
    if (prepared_.mmap_ptr != nullptr) {
        ::munmap(prepared_.mmap_ptr, segment_size_);
    }
    if (prepared_.fd >= 0) {
        ::close(prepared_.fd);
    }
    prepared_ = OpenedSegment{};
}

bool WalWriter::wait_until_prepared(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const PrepState state = prep_state_.load(std::memory_order_acquire);
        if (state == PrepState::Ready) {
            return true;
        }
        if (state == PrepState::Failed) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return prep_state_.load(std::memory_order_acquire) == PrepState::Ready;
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

    // The helper owns requested_segment_ and prepared_ for as long as the state says Requested,
    // and owns the file it is building. Nothing here may touch any of them until it is done.
    if (prep_state_.load(std::memory_order_acquire) == PrepState::Requested) {
        await_helper();
    }

    if (prep_state_.load(std::memory_order_acquire) == PrepState::Ready && prepared_.seg_num == next) {
        // The common path, and the point of the whole arrangement: no open, no fill, no mmap on
        // this thread, so nothing here can wait on the filesystem.
        adopt(prepared_);
        prepared_ = OpenedSegment{};
        prep_state_.store(PrepState::Idle, std::memory_order_release);
    } else {
        // Not ready. Make it here rather than wait, and never append into a segment whose
        // blocks do not exist -- that is the defect this exists to prevent, not a fallback.
        release_prepared_if_ready();
        open_segment(next);
    }

    write_offset_ = 0;
    request_preparation(current_segment_ + 1);
}

// append() -- the commit act

void WalWriter::append(int64_t record_id, const void* payload, size_t size) {
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
