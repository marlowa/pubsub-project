// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/MappedSlotStore.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StoreCapacityDiffersException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

namespace {
constexpr uint32_t store_magic = 0x534C4F54u; // "SLOT"
constexpr uint32_t store_version = 5;
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
    // What the owner's records mean, as the owner numbers it. See open().
    uint32_t payload_format;
    // Values the owner keeps beside the published position, which this class does not
    // interpret. See set_owner_value().
    std::atomic<int64_t> owner_values[owner_value_count];
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

void MappedSlotStore::check_existing_header(int fd, const std::string& path, uint32_t payload_size, SlotIndex slot_count, uint32_t payload_format) {
    // Something is already in this file, so it is not a new store however unreadable it turns out to be, and it must not
    // be written over. A store whose header was damaged is the same file, from here, as one that was never a store, and
    // taking either over would discard every record it held while reporting a new store.
    //
    // The fields are read from the file at their places in Header, rather than through a mapping, so that nothing about
    // the file changes before it is accepted. A file too short to hold them is not a store.
    std::array<uint8_t, sizeof(Header)> bytes{};
    const ssize_t got = ::pread(fd, bytes.data(), bytes.size(), 0);
    if (got < 0) {
        throw PubSubItcException("MappedSlotStore: reading the header of " + path + ": " + StringUtils::get_errno_string());
    }
    if (static_cast<size_t>(got) < bytes.size()) {
        throw PubSubItcException("MappedSlotStore: " + path + " is not a store this understands: it holds " + std::to_string(got) +
                                 " byte(s), fewer than a store's header");
    }
    const auto field = [&bytes](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const uint32_t found_magic = field(offsetof(Header, magic));
    const uint32_t found_version = field(offsetof(Header, version));
    const uint32_t found_payload_size = field(offsetof(Header, payload_size));
    const uint32_t found_slot_count = field(offsetof(Header, slot_count));
    const uint32_t found_payload_format = field(offsetof(Header, payload_format));

    if (found_magic != store_magic) {
        throw PubSubItcException("MappedSlotStore: " + path + " is not a store this understands: it begins " + std::to_string(found_magic) +
                                 " where a store begins " + std::to_string(store_magic));
    }

    // Refuse rather than reinterpret. A store read with the wrong record size produces records that are wrong in ways
    // nothing downstream can detect.
    if (found_version != store_version || found_payload_size != payload_size || found_payload_format != payload_format) {
        const std::string held = std::to_string(found_version) + "/" + std::to_string(found_payload_size) + "/" + std::to_string(found_slot_count) + "/" +
                                 std::to_string(found_payload_format);
        const std::string want =
            std::to_string(store_version) + "/" + std::to_string(payload_size) + "/" + std::to_string(slot_count) + "/" + std::to_string(payload_format);
        throw PubSubItcException("MappedSlotStore: " + path + " holds version/record size/slot count/record format " + held + ", and this asks for " + want);
    }

    // Everything else agrees, so the store is intact and only its size differs. That is refused too, with its own
    // exception, so that a caller can tell an intact store opened with the wrong capacity from a damaged one.
    if (found_slot_count != slot_count) {
        throw StoreCapacityDiffersException("MappedSlotStore: " + path + " was created holding " + std::to_string(found_slot_count) +
                                                " record(s), and this asks for " + std::to_string(slot_count),
                                            found_slot_count, slot_count);
    }
}

bool MappedSlotStore::open(const std::string& path, uint32_t payload_size, SlotIndex slot_count, uint32_t payload_format) {
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

    // An existing file is checked before anything changes it. Setting its length comes after, because a store refused
    // here must be left exactly as it was: shortening a store opened with fewer records than it holds would cut off the
    // records past the new end, and the store would then be damaged as well as refused.
    if (existed) {
        try {
            check_existing_header(fd, path, payload_size, slot_count, payload_format);
        } catch (...) {
            ::close(fd);
            throw;
        }
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

    if (existed) {
        return true;
    }

    Header* hdr = header();

    // Nothing was there, so this is a new store and it starts empty.
    hdr->magic = store_magic;
    hdr->version = store_version;
    hdr->payload_size = payload_size;
    hdr->slot_count = slot_count;
    hdr->published.store(0, std::memory_order_relaxed);
    hdr->payload_format = payload_format;
    for (std::atomic<int64_t>& value : hdr->owner_values) {
        value.store(0, std::memory_order_relaxed);
    }
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
    // Write a byte back to every page rather than reading one from it.
    //
    // Reading was the first attempt and it warms nothing. The region is created with ftruncate,
    // which sets the size and allocates not one block, so most of it is holes -- and a READ of a
    // hole allocates nothing: the kernel maps the shared zero page and returns. The delay this
    // exists to move to startup was therefore not moved. It still happens, later, on the first
    // WRITE to each slot, which is on the order path.
    //
    // Reading is not enough even where the blocks already exist. A shared file-backed page is
    // faulted in read-only so that the kernel can trap the first write through page_mkwrite for
    // writeback accounting and, under delayed allocation, block reservation. A read moves one
    // fault to startup and leaves the other where it was.
    //
    // Writing the byte back takes both, and takes them whether the page was a hole or not --
    // which is why this does not branch on whether the region is new. "New or existing" is the
    // wrong axis: a region that has been used but only partly written still holds holes, and it
    // is the hole that costs rather than the file's age.
    //
    // The byte written is the byte read, so no content changes and this stays const in every
    // sense a caller can observe. What it does cost is that startup dirties the whole region and
    // the kernel writes it back -- paid before the component reports itself ready, which is the
    // trade R-0121 asks for, and stated here rather than discovered in a run.
    //
    // The same lesson, measured, is in WalWriter::fill_new_segment: writing the bytes is what
    // works, and fallocate() is not sufficient because ext4 reserves extents UNWRITTEN and the
    // first write must convert them, which is itself journalled. See docs/bug_list.md, BUG-0071
    // and BUG-0070.
    volatile uint8_t* p = static_cast<uint8_t*>(base_);
    for (size_t offset = 0; offset < mapped_size_; offset += page_size) {
        p[offset] = p[offset];
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

void MappedSlotStore::set_owner_value(size_t index, int64_t value) {
    if (index >= owner_value_count) {
        throw PreconditionAssertion("MappedSlotStore::set_owner_value: index " + std::to_string(index) + " is not below " + std::to_string(owner_value_count),
                                    __FILE__, __LINE__);
    }
    header()->owner_values[index].store(value, std::memory_order_relaxed);
}

int64_t MappedSlotStore::owner_value(size_t index) const {
    if (index >= owner_value_count) {
        throw PreconditionAssertion("MappedSlotStore::owner_value: index " + std::to_string(index) + " is not below " + std::to_string(owner_value_count),
                                    __FILE__, __LINE__);
    }
    return header()->owner_values[index].load(std::memory_order_relaxed);
}

void MappedSlotStore::mark_unusable() {
    // Any word other than the magic: open() then refuses the file, saying what it found.
    header()->magic = 0;
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
