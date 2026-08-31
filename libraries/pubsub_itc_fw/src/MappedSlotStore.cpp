// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/MappedSlotStore.hpp>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

namespace {
constexpr uint32_t store_magic = 0x534C4F54u; // "SLOT"
constexpr uint32_t store_version = 2;
constexpr uint32_t slot_free = 0;
constexpr uint32_t slot_live = 1;
constexpr size_t page_size = 4096;
} // namespaces

// Laid out so that every field is a value: the file is read again at whatever address the
// operating system chooses, so an address stored in it would mean nothing.
struct MappedSlotStore::Header {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    uint32_t slot_count;
    // Everything at or below this is settled. Read first by whoever opens the store, and
    // the only field written on the path that records anything.
    std::atomic<int64_t> published;
    // A hint, and rebuilt from the slots after any unclean stop.
    std::atomic<uint32_t> free_head;
    uint32_t filler;
    // When the owner last said it was working, as a wall-clock time. Whoever opens the store
    // next subtracts it from the current time to learn how long nothing was tending it.
    //
    // Deliberately not written by acquire() or commit(). A store that is idle because nothing
    // is happening is not a store nobody is tending, and stamping this on writes alone would
    // read a quiet hour followed by a two-second restart as an hour of absence.
    std::atomic<int64_t> alive_at_ns;
};

struct MappedSlotStore::SlotHeader {
    std::atomic<uint32_t> state;
    uint32_t next_free;
    int64_t seq_no;
};

MappedSlotStore::~MappedSlotStore() {
    close();
}

MappedSlotStore::Header* MappedSlotStore::header() {
    return static_cast<Header*>(base_);
}

const MappedSlotStore::Header* MappedSlotStore::header() const {
    return static_cast<const Header*>(base_);
}

MappedSlotStore::SlotHeader* MappedSlotStore::slot(SlotIndex index) {
    return reinterpret_cast<SlotHeader*>(static_cast<uint8_t*>(base_) + sizeof(Header) + index * slot_stride_);
}

const MappedSlotStore::SlotHeader* MappedSlotStore::slot(SlotIndex index) const {
    return reinterpret_cast<const SlotHeader*>(static_cast<const uint8_t*>(base_) + sizeof(Header) + index * slot_stride_);
}

bool MappedSlotStore::open(const std::string& path, uint32_t payload_size, SlotIndex slot_count) {
    if (payload_size == 0 || slot_count == 0 || slot_count == no_slot) {
        throw PreconditionAssertion("MappedSlotStore: a store needs a non-zero record size and a slot count below no_slot", __FILE__, __LINE__);
    }

    payload_size_ = payload_size;
    slot_count_ = slot_count;
    slot_stride_ = sizeof(SlotHeader) + payload_size;
    // Round the stride so that each slot's atomics stay aligned.
    slot_stride_ = (slot_stride_ + 7u) & ~static_cast<size_t>(7u);

    const size_t wanted = sizeof(Header) + slot_stride_ * slot_count;
    mapped_size_ = (wanted + page_size - 1) & ~(page_size - 1);

    struct stat st {};
    const bool existed = ::stat(path.c_str(), &st) == 0 && st.st_size > 0;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw PubSubItcException("MappedSlotStore: open(" + path + "): " + StringUtils::get_errno_string());
    }

    if (::ftruncate(fd, static_cast<off_t>(mapped_size_)) != 0) {
        const std::string reason = StringUtils::get_errno_string();
        ::close(fd);
        throw PubSubItcException("MappedSlotStore: ftruncate(" + path + "): " + reason);
    }

    base_ = ::mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);

    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        throw PubSubItcException("MappedSlotStore: mmap(" + path + "): " + StringUtils::get_errno_string());
    }

    Header* hdr = header();

    if (existed) {
        // Something is already in this file, so it is not a new store however unreadable it
        // turns out to be, and it must not be written over.
        //
        // It used to be taken over when the magic did not match, on the reasoning that a file
        // which was never a store has nothing worth keeping. That is true of a file nobody
        // wrote, and false of a store whose header was damaged -- which is the same file from
        // here. Taking it over discarded every record it held and reported a new store, so the
        // one case that most needs to be noticed was the one that looked most ordinary.
        if (hdr->magic != store_magic) {
            // Read before close(), which unmaps the region hdr points into.
            const uint32_t found = hdr->magic;
            close();
            throw PubSubItcException("MappedSlotStore: " + path + " is not a store this understands: it begins " + std::to_string(found) +
                                     " where a store begins " + std::to_string(store_magic));
        }

        // Refuse rather than reinterpret. A store read with the wrong record size produces
        // records that are wrong in ways nothing downstream can detect.
        if (hdr->version != store_version || hdr->payload_size != payload_size || hdr->slot_count != slot_count) {
            const std::string held = std::to_string(hdr->version) + "/" + std::to_string(hdr->payload_size) + "/" + std::to_string(hdr->slot_count);
            const std::string want = std::to_string(store_version) + "/" + std::to_string(payload_size) + "/" + std::to_string(slot_count);
            close();
            throw PubSubItcException("MappedSlotStore: " + path + " holds version/record size/slot count " + held + ", and this asks for " + want);
        }
        return true;
    }

    // Nothing was there, so this is a new store and it starts empty.
    hdr->magic = store_magic;
    hdr->version = store_version;
    hdr->payload_size = payload_size;
    hdr->slot_count = slot_count;
    hdr->published.store(0, std::memory_order_relaxed);
    hdr->filler = 0;
    hdr->alive_at_ns.store(0, std::memory_order_relaxed);
    for (SlotIndex i = 0; i < slot_count; ++i) {
        SlotHeader* s = slot(i);
        s->state.store(slot_free, std::memory_order_relaxed);
        s->seq_no = 0;
        s->next_free = (i + 1 < slot_count) ? (i + 1) : no_slot;
    }
    hdr->free_head.store(0, std::memory_order_release);
    return false;
}

void MappedSlotStore::close() {
    if (base_ != nullptr) {
        ::munmap(base_, mapped_size_);
        base_ = nullptr;
    }
    mapped_size_ = 0;
}

MappedSlotStore::SlotIndex MappedSlotStore::capacity() const {
    return slot_count_;
}

uint32_t MappedSlotStore::payload_size() const {
    return payload_size_;
}

void MappedSlotStore::warm() const {
    // Read one byte from each page. Reading is enough: the delay being moved is the kernel
    // finding the page, and it does that whether the access reads or writes.
    volatile const uint8_t* p = static_cast<const uint8_t*>(base_);
    for (size_t offset = 0; offset < mapped_size_; offset += page_size) {
        (void)p[offset];
    }
}

MappedSlotStore::SlotIndex MappedSlotStore::acquire() {
    Header* hdr = header();
    const SlotIndex head = hdr->free_head.load(std::memory_order_relaxed);
    if (head == no_slot) {
        return no_slot;
    }
    hdr->free_head.store(slot(head)->next_free, std::memory_order_relaxed);
    return head;
}

uint8_t* MappedSlotStore::payload(SlotIndex index) {
    return reinterpret_cast<uint8_t*>(slot(index)) + sizeof(SlotHeader);
}

const uint8_t* MappedSlotStore::payload(SlotIndex index) const {
    return reinterpret_cast<const uint8_t*>(slot(index)) + sizeof(SlotHeader);
}

void MappedSlotStore::commit(SlotIndex index, int64_t seq_no) {
    SlotHeader* s = slot(index);
    s->seq_no = seq_no;
    // Release: whoever sees this slot live also sees the record and the sequence number.
    s->state.store(slot_live, std::memory_order_release);
}

void MappedSlotStore::release(SlotIndex index) {
    Header* hdr = header();
    SlotHeader* s = slot(index);
    s->state.store(slot_free, std::memory_order_release);
    s->next_free = hdr->free_head.load(std::memory_order_relaxed);
    hdr->free_head.store(index, std::memory_order_relaxed);
}

void MappedSlotStore::mark_alive(int64_t wall_time_ns) {
    header()->alive_at_ns.store(wall_time_ns, std::memory_order_relaxed);
}

int64_t MappedSlotStore::alive_at_ns() const {
    return header()->alive_at_ns.load(std::memory_order_relaxed);
}

void MappedSlotStore::publish(int64_t seq_no) {
    header()->published.store(seq_no, std::memory_order_release);
}

int64_t MappedSlotStore::published() const {
    return header()->published.load(std::memory_order_acquire);
}

bool MappedSlotStore::is_live(SlotIndex index) const {
    return slot(index)->state.load(std::memory_order_acquire) == slot_live;
}

int64_t MappedSlotStore::slot_seq_no(SlotIndex index) const {
    return slot(index)->seq_no;
}

bool MappedSlotStore::is_recoverable(SlotIndex index) const {
    const int64_t stamp = slot_seq_no(index);
    return is_live(index) && stamp > 0 && stamp <= published();
}

MappedSlotStore::SlotIndex MappedSlotStore::rebuild_free_list() {
    Header* hdr = header();
    SlotIndex recoverable = 0;
    SlotIndex head = no_slot;

    // Backwards, so the list comes out in ascending order and a fresh store hands out
    // slot 0 first -- which makes a scan of a lightly used store touch fewer pages.
    for (SlotIndex i = slot_count_; i > 0; --i) {
        const SlotIndex index = i - 1;
        if (is_recoverable(index)) {
            ++recoverable;
            continue;
        }
        // Not recoverable: either free, or written by work that never finished. Both are
        // the store's to hand out again, and anything unfinished is produced again by
        // whoever was producing it.
        SlotHeader* s = slot(index);
        s->state.store(slot_free, std::memory_order_relaxed);
        s->next_free = head;
        head = index;
    }

    hdr->free_head.store(head, std::memory_order_release);
    return recoverable;
}

} // namespaces
