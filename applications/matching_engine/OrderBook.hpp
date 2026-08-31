#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <string>
#include <type_traits>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/IncrementalRehashMap.hpp>
#include <pubsub_itc_fw/MappedSlotStore.hpp>

#include "OrderEntry.hpp"
#include "OrderKey.hpp"

namespace matching_engine {

/**
 * @brief The orders the venue is holding, in a form that outlives the process holding them.
 *
 * There is one copy of each order and it lives in a memory-mapped region of fixed-size
 * records. This class owns that region together with the map from an order's identity to the
 * record holding it, so a lookup is a hash lookup followed by index arithmetic.
 *
 * The alternative -- a region kept alongside a map that also held the orders -- writes every
 * order twice on the accept path and lets the two disagree, which is a class of defect with
 * no upper bound on how confusing it gets.
 *
 * **The caller's ordering is the whole of the safety**, and this class cannot enforce it:
 *
 *   1. add() or remove(), which writes the record
 *   2. do the outward-visible thing, which is emitting the execution report
 *   3. publish(seq_no)
 *
 * A death anywhere before step 3 leaves the change above the published position, where
 * recovery ignores it, and the sequencer's tail runs the same work again. Publishing before
 * emitting would trade a repeated report, which a member can recognise, for a silent loss,
 * where the venue holds an order the member was never told about.
 *
 * Not thread-safe. It belongs to the matching engine's thread, which is also where the
 * published position must be written -- moving that elsewhere makes the position lag by an
 * arbitrary amount and lengthens what a recovery has to replay.
 *
 * See docs/durability/open_order_checkpoint.md.
 */
class OrderBook {
  public:
    using SlotIndex = pubsub_itc_fw::MappedSlotStore::SlotIndex;

    ~OrderBook();

    /**
     * @brief Builds an empty book.
     * @param[in] reporter Told about the map's own growth; may be null. Fixed here rather than
     *                     at open(), because the map takes it at construction.
     */
    explicit OrderBook(pubsub_itc_fw::AllocationGrowthReporter* reporter = nullptr);

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    /**
     * @brief Opens the region and sizes the map.
     * @param[in] region_path     File to map.
     * @param[in] region_capacity Records the region holds: the most orders that can be open at once.
     * @param[in] map_capacity    Elements to reserve in the map.
     * @return true when an existing region was opened rather than a new one created.
     * @throws pubsub_itc_fw::PubSubItcException if the file cannot be mapped, or holds a region
     *         written with a different record size, record count or version.
     *
     * Opening an existing region does not read what is in it: that is recovery, and it happens
     * only when the caller has decided it wants what the region holds.
     */
    bool open(const std::string& region_path, SlotIndex region_capacity, size_t map_capacity);

    /** @brief True when the region is mapped. Everything below requires it. */
    [[nodiscard]] bool is_open() const {
        return store_.is_open();
    }

    /** @brief The most orders that can be open at once. */
    [[nodiscard]] SlotIndex region_capacity() const {
        return store_.capacity();
    }

    /** @brief Touches every page of the region, so that no order pays the first fetch. See R-0121. */
    void warm() const {
        store_.warm();
    }

    /** @brief True when an order with this identity is already open. */
    [[nodiscard]] bool contains(const OrderKey& key) const;

    /** @brief The open order with this identity, or null. Valid until the book next changes. */
    [[nodiscard]] const OrderEntry* find(const OrderKey& key) const;

    /**
     * @brief Records an open order, stamped with the sequence number that placed it.
     * @return false when the region is full, in which case nothing was recorded.
     *
     * The identity is written into the record as well as used to file it, because a record
     * read back after a restart has to say which order it is.
     *
     * A full region means the order must be refused. Growing the region would fault pages in
     * on the engine's thread, and recycling the oldest record would silently lose an order the
     * venue had accepted -- a visible refusal is the only one of the three a member can act on.
     */
    [[nodiscard]] bool add(const OrderKey& key, const OrderEntry& entry, int64_t seq_no);

    /**
     * @brief Records an open order, replacing whatever was filed under this identity.
     * @return false when the region is full and there was nothing to replace.
     */
    [[nodiscard]] bool add_or_replace(const OrderKey& key, const OrderEntry& entry, int64_t seq_no);

    /** @brief Removes an open order. @return true when there was one to remove. */
    bool remove(const OrderKey& key);

    /** @brief Removes every open order. */
    void clear();

    /** @brief What a recovery found in the region. */
    struct Recovery {
        size_t orders{};                ///< Orders the region held at or below its published position.
        size_t discarded{};             ///< Records above it, or never made live: work that was not finished.
        int64_t published{};            ///< How far the region says it is current.
        int64_t highest_order_id_num{}; ///< So a successor does not reissue an order number.
    };

    /**
     * @brief Reads the region back and rebuilds the book from it.
     *
     * Every record at or below the published position is filed again under the identity it
     * carries. Anything above it was written by work that had not finished, and is left to
     * whoever produced it to produce again; the free list is rebuilt from what is left rather
     * than trusted, because a process that died mid-change can leave it holding a dangling
     * index or a cycle.
     *
     * Reads the whole region, so it touches every page: after this there is nothing left for
     * warm() to do. Costs one pass over the region, which is fixed by its size and not by the
     * orders in it.
     *
     * Call it on an empty book, once, before anything else changes it.
     */
    Recovery recover();

    /**
     * @brief States that the region holds every change up to this sequence number.
     *
     * Call it after the change has been made visible outside the venue, never before.
     */
    void publish(int64_t seq_no) {
        store_.publish(seq_no);
    }

    /** @brief The sequence number this book last published. */
    [[nodiscard]] int64_t published() const {
        return store_.published();
    }

    /**
     * @brief Records that the engine was able to match at this wall-clock time.
     *
     * Call it on a timer. Not from the accept or cancel path: a book that is idle because
     * nothing is being traded is not a book nobody is tending, and stamping this on orders
     * would read a quiet hour followed by a restart as an hour of absence.
     */
    void mark_alive(int64_t wall_time_ns) {
        store_.mark_alive(wall_time_ns);
    }

    /** @brief When the previous owner was last able to match, or zero if it never said. */
    [[nodiscard]] int64_t alive_at_ns() const {
        return store_.alive_at_ns();
    }

    [[nodiscard]] size_t size() const {
        return index_.size();
    }
    [[nodiscard]] size_t capacity() const {
        return index_.capacity();
    }
    [[nodiscard]] bool is_migrating() const {
        return index_.is_migrating();
    }

    /**
     * @brief Calls fn(key, entry) for every open order, in no particular order.
     *
     * The book must not be changed from within fn.
     */
    template <typename Fn> void for_each(Fn&& fn) const {
        for (const auto& kv : index_) {
            fn(kv.first, *entry_at(kv.second));
        }
    }

  private:
    // A record is read back at a different address than it was written at, so it must be
    // copyable as bytes and hold nothing that reaches outside itself.
    static_assert(std::is_trivially_copyable<OrderEntry>::value, "an OrderEntry is held as bytes in the region and must be trivially copyable");

    [[nodiscard]] const OrderEntry* entry_at(SlotIndex slot) const;
    [[nodiscard]] OrderEntry* entry_at(SlotIndex slot);

    pubsub_itc_fw::MappedSlotStore store_;

    // From an order's identity to the record holding it.
    //
    // On IncrementalRehashMap rather than a std-style map because growth must not stall the
    // reactor callback thread. A conventional hash map rehashes its whole table inside the one
    // insert that crosses the load factor: at 2^23 orders that was measured at over a second,
    // during which this thread matches nothing. IncrementalRehashMap spreads the same work
    // across the following operations, a fixed few slots at a time.
    pubsub_itc_fw::IncrementalRehashMap<OrderKey, SlotIndex, OrderKeyHash> index_;
};

} // namespaces
