// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Fixed-size records in a memory-mapped file, outliving the process that wrote them.
 *
 * A store of equally sized slots, each either free or live, held in a file the operating
 * system maps into the process. Writing a slot is writing memory; the kernel writes the
 * pages back on its own schedule. When the process dies the pages remain, so the next
 * process to map the same file finds the records already there.
 *
 * **What it is for.** A component whose state exists only in its own memory loses that
 * state when it dies. Rebuilding it by replaying a log costs time proportional to
 * everything that has happened, where this costs time proportional to what is currently
 * held. See `docs/durability/open_order_checkpoint.md`.
 *
 * **What it survives.** The process dying. Not the machine dying and not power loss:
 * nothing here flushes, deliberately, because flushing on the path that writes records
 * would cost the latency the arrangement exists to protect. Surviving a machine is what a
 * second machine is for.
 *
 * **Why nothing here is a pointer.** Every reference between slots is an index, and the
 * free list lives in the file as indices. A file mapped again lands wherever the operating
 * system puts it, so an address written by one process means nothing to the next. That is
 * also why the live structures of a component cannot simply be persisted as they stand.
 *
 * **The order of writes is the whole of the safety, and belongs to the caller:**
 *
 *   1. `acquire()` a slot
 *   2. write the record into `payload()`
 *   3. `commit()` it with the sequence number it belongs to
 *   4. do whatever the record makes true -- send a message, answer somebody
 *   5. `publish()` the sequence number
 *
 * A death before step 5 leaves the record above the published position, where a reader
 * ignores it, and whatever produced the record is expected to produce it again. A death
 * between 4 and 5 therefore repeats step 4, which is why a caller that sends messages must
 * be able to mark a repeat. Publishing before step 4 would trade that repeat for a record
 * held with nobody told about it.
 *
 * **The free list is a hint and is never trusted after a crash.** It is mutated on every
 * acquire and release, so a process dying mid-update can leave it holding a dangling index
 * or a cycle. A reader scans the slots and calls `rebuild_free_list()`, which costs nothing
 * extra because the scan is happening anyway.
 *
 * Not thread-safe. One writer, which is the component that owns the state. Everything
 * except open(), close() and is_open() requires an open store.
 */
class MappedSlotStore {
  public:
    using SlotIndex = uint32_t;

    /// Returned by acquire() when every slot is in use.
    static constexpr SlotIndex no_slot = 0xFFFFFFFFu;

    ~MappedSlotStore();
    MappedSlotStore() = default;

    MappedSlotStore(const MappedSlotStore&) = delete;
    MappedSlotStore& operator=(const MappedSlotStore&) = delete;
    MappedSlotStore(MappedSlotStore&&) = delete;
    MappedSlotStore& operator=(MappedSlotStore&&) = delete;

    /**
     * @brief Open the store at @p path, creating it if it is not there.
     *
     * @return true when an existing store was opened, false when one was created. A caller
     *         that gets true has records to recover and must scan before acquiring
     *         anything; one that gets false has an empty store.
     *
     * Throws when the file exists but was written with a different record size, slot count
     * or version -- a store that cannot be read as the caller expects is refused rather
     * than reinterpreted, because reinterpreting it silently produces records that are
     * wrong in ways nothing downstream can detect.
     */
    bool open(const std::string& path, uint32_t payload_size, SlotIndex slot_count);

    void close();

    [[nodiscard]] bool is_open() const {
        return base_ != nullptr;
    }
    [[nodiscard]] SlotIndex capacity() const;
    [[nodiscard]] uint32_t payload_size() const;

    /// Touch every page so that the delays of first use are taken now rather than later.
    /// A component doing this before it reports itself ready has already paid them.
    void warm() const;

    // ---- writing -----------------------------------------------------------------

    /// Take a free slot, or no_slot when the store is full. The slot is not live until
    /// commit(); a process dying in between leaks it, and the next scan reclaims it.
    [[nodiscard]] SlotIndex acquire();

    /// The record's bytes. payload_size() of them.
    [[nodiscard]] uint8_t* payload(SlotIndex slot);
    [[nodiscard]] const uint8_t* payload(SlotIndex slot) const;

    /// Make the slot live, stamped with the sequence number the record belongs to.
    /// Call after the payload is written and before publish().
    void commit(SlotIndex slot, int64_t seq_no);

    /// Return a live slot to the free list.
    void release(SlotIndex slot);

    /// Say that everything at or below @p seq_no is settled. Read back by whoever opens
    /// the store next, which trusts no slot stamped above it.
    void publish(int64_t seq_no);

    /// Record that whoever owns this store was working at this wall-clock time.
    ///
    /// Nothing here interprets it. It exists so that the next process to open the store can
    /// tell how long the store went untended, which is a different question from how long ago
    /// it was last written to: a store can be idle for an hour because nothing was happening.
    /// So call it on a timer, and not from the path that records anything.
    void mark_alive(int64_t wall_time_ns);

    // ---- reading, after opening an existing store ---------------------------------

    [[nodiscard]] int64_t published() const;

    /// The last time mark_alive() was called, or zero if it never was.
    [[nodiscard]] int64_t alive_at_ns() const;
    [[nodiscard]] bool is_live(SlotIndex slot) const;
    [[nodiscard]] int64_t slot_seq_no(SlotIndex slot) const;

    /// Live, and stamped at or below the published position. Slots above it were written
    /// by work that had not finished and are for whoever produced them to produce again.
    /// Sequence numbers are counted from one; a slot stamped zero is not recoverable,
    /// because a new store publishes zero and would otherwise trust it.
    [[nodiscard]] bool is_recoverable(SlotIndex slot) const;

    /// Rebuild the free list from every slot that is not recoverable, and return how many
    /// are. Call once after opening an existing store and before acquiring anything.
    SlotIndex rebuild_free_list();

  private:
    struct Header;
    struct SlotHeader;

    [[nodiscard]] Header* header();
    [[nodiscard]] const Header* header() const;
    [[nodiscard]] SlotHeader* slot(SlotIndex index);
    [[nodiscard]] const SlotHeader* slot(SlotIndex index) const;

    void* base_{nullptr};
    size_t mapped_size_{0};
    uint32_t payload_size_{0};
    SlotIndex slot_count_{0};
    size_t slot_stride_{0};
};

} // namespaces
