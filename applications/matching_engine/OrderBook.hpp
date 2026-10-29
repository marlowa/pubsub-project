#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/IncrementalRehashMap.hpp>
#include <pubsub_itc_fw/MappedSlotStore.hpp>

#include "OrderEntry.hpp"
#include "OrderKey.hpp"

namespace matching_engine {

/// Whether trading is halted. See OrderBook::set_halted.
enum class TradingHalted { no, yes };

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

    /**
     * @brief The format of the records the region holds, written in the region's header and checked when it is opened.
     *
     * Raised whenever what an OrderEntry's bytes mean changes, even when its size does not, so that a
     * region written by a build that stored something else is refused rather than read as though it
     * held this. Format 1 holds, in each record, the sequence number of the new order that placed it.
     */
    static constexpr uint32_t record_format = 1;

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

    /**
     * @brief Makes the calling thread the owner of the book's map, in builds with thread checks.
     *
     * The book is opened and recovered by the thread that constructs the matching engine, and is
     * used from then on by the engine's own application thread. That thread calls this before it
     * first uses the book. Does nothing in a build without thread checks.
     */
    void hand_over_ownership() {
        index_.hand_over_ownership();
    }

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

    /** @brief Removes an open order. @return true when there was one to remove. */
    bool remove(const OrderKey& key);

    /** @brief Removes every open order. */
    void clear();

    /** @brief What a recovery found in the region. */
    struct Recovery {
        size_t orders{};     ///< Orders the region held at or below its published position.
        size_t discarded{};  ///< Records above it, or never made live: work that was not finished.
        int64_t published{}; ///< How far the region says it is current.
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

    /**
     * @brief Says that the region must not be used again, so that the next process to open it rebuilds the book from the
     *        WAL. This process goes on using it.
     */
    void mark_unusable() {
        store_.mark_unusable();
    }

    /** @brief The sequence number this book last published. */
    [[nodiscard]] int64_t published() const {
        return store_.published();
    }

    /**
     * @brief Records whether trading is halted, in the region beside the published position.
     *
     * Halting is part of the state the commands build, like the orders, so it is kept where they are: an engine that
     * restarts resumes from the published position and is not sent the command that halted trading again, so it finds
     * the halt here instead. Call it before publishing the position of the command that set it. Kept in memory as well,
     * so that a book with no region still answers halted().
     */
    void set_halted(TradingHalted halted) {
        halted_ = halted == TradingHalted::yes;
        if (store_.is_open()) {
            store_.set_owner_value(halted_ ? 1 : 0);
        }
    }

    /** @brief Whether trading is halted, as last recorded in this process or found in the region when it was opened. */
    [[nodiscard]] bool halted() const {
        return halted_;
    }

    [[nodiscard]] size_t size() const {
        return index_.size();
    }

    /**
     * @brief What is wrong with the book, or an empty string if nothing is: the region and the map must agree.
     *
     * Every order in the map must be filed under a record in use that holds that order and be found by a lookup of its
     * key, and the region must hold no record in use that the map does not file. Reads every record of the region, so it is for tests, such as the
     * fuzzing of the engine's state machine, and not for the order path.
     */
    [[nodiscard]] std::string consistency_problem() const;
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

    /**
     * @brief Calls fn(entry) for every open order, in the order of the sequence numbers of the new orders that placed them.
     *
     * Every engine that applied the same commands visits the orders in the same order, which for_each
     * does not promise: the map's order depends on the order the keys were inserted in and on how far a
     * rehash has got, and an engine that rebuilt its map from the region at a restart inserted them in a
     * different order from one that accepted them live.
     *
     * Collects each order's sequence number and record number, sorts them, and visits the records: it
     * allocates 16 bytes for each open order for the length of the call, and sorts them, which for a
     * hundred million orders takes seconds. It is for a cancel of every order, which is rare, not for
     * the order path. The book must not be changed from within fn.
     */
    template <typename Fn> void for_each_in_order_placed(Fn&& fn) const {
        std::vector<std::pair<int64_t, SlotIndex>> placed;
        placed.reserve(index_.size());
        for (const auto& kv : index_) {
            placed.emplace_back(entry_at(kv.second)->placed_at_seq_no, kv.second);
        }
        std::sort(placed.begin(), placed.end());
        for (const auto& order : placed) {
            fn(*entry_at(order.second));
        }
    }

    /**
     * @brief Takes off the book every order placed at or before a sequence number, calling fn(entry) for each just before
     * it is removed, in the order the orders were placed.
     *
     * As for_each_in_order_placed, every engine that applied the same commands visits the same orders in the same
     * order, and the list it sorts takes 16 bytes for each order removed for the length of the call. The orders are
     * removed from the list it sorted, not while walking the map, so the map is never changed under a walk. fn must not
     * change the book.
     *
     * @param[in] placed_through The highest sequence number of a new order whose order is removed.
     * @param[in] fn             Called with each order's record, which is valid only for the call.
     * @return How many orders were removed.
     */
    template <typename Fn> size_t remove_placed_through(int64_t placed_through, Fn&& fn) {
        std::vector<std::pair<int64_t, SlotIndex>> placed;
        for (const auto& kv : index_) {
            const int64_t seq_no = entry_at(kv.second)->placed_at_seq_no;
            if (seq_no <= placed_through) {
                placed.emplace_back(seq_no, kv.second);
            }
        }
        std::sort(placed.begin(), placed.end());
        for (const auto& order : placed) {
            const OrderEntry* entry = entry_at(order.second);
            fn(*entry);
            index_.erase(OrderKey::make(entry->session, entry->get_cl_ord_id()));
            store_.release(order.second);
        }
        return placed.size();
    }

  private:
    // A record is read back at a different address than it was written at, so it must be
    // copyable as bytes and hold nothing that reaches outside itself.
    static_assert(std::is_trivially_copyable<OrderEntry>::value, "an OrderEntry is held as bytes in the region and must be trivially copyable");

    [[nodiscard]] const OrderEntry* entry_at(SlotIndex slot) const;
    [[nodiscard]] OrderEntry* entry_at(SlotIndex slot);

    pubsub_itc_fw::MappedSlotStore store_;
    // Whether trading is halted; see set_halted().
    bool halted_{false};

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
