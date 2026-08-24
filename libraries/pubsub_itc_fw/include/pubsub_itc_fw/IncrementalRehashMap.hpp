#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <utility>

// Thread-confinement checking is a debug, ASan and coverage build facility: it costs a
// thread-local read on every operation, which a hot path should not pay to verify something
// the design already guarantees. PUBSUB_ITC_FW_THREAD_CHECKS is set by the top-level
// CMakeLists for exactly those build types. The two includes it needs are guarded separately
// so that each still sits in the section the include-order rule puts it in.
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <thread>
#endif

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#endif

namespace pubsub_itc_fw {

/**
 * @file IncrementalRehashMap.hpp
 * @brief A hash map that grows without ever rehashing the whole table in one operation.
 *
 * The framework's pool and slab allocators serve objects with a message lifecycle. Nothing
 * served long-lived application state that GROWS -- an order book, a session table, a
 * subscription registry -- and every such structure ends up a hash map on a reactor callback
 * thread. A conventional map doubles by rehashing every entry in the operation unlucky enough
 * to cross the threshold, so the cost of growth is paid by one message while every other
 * message waits behind it.
 *
 * That is not a theoretical concern. The matching engine's order book was a `tsl::robin_map`,
 * and it stalled its callback thread for 96 ms at 2^21 entries, 733 ms at 2^22 and over a
 * second at 2^23, at which point the pipeline did not recover and 1,167,392 of 9,556,000 orders
 * were never accepted. Memory was never short: 16 GB free on a 31 GB machine. The stalls landed
 * on exact powers of two and nowhere else. See docs/operations/trading_day_load.md.
 *
 * The book has since been moved onto this container, so that account is history rather than a
 * description of the venue as it stands. It is kept because it is the measurement this class
 * exists to answer, and because the shape of the failure -- a tail excursion on exact powers of
 * two, with p50 and p90 untouched throughout -- is what to look for if it ever returns.
 *
 * ## What this does instead
 *
 * When the table must grow, a second table is allocated and the entries are moved a few at a
 * time, by the operations that follow. Every insert and erase carries a bounded share of the
 * work -- `migration_slots_per_operation`, 8 by default -- so no operation pays for the table.
 * Both tables are searched while a migration is in flight, so a lookup always finds an entry
 * wherever it currently lives.
 *
 * The worst case per operation is therefore a probe plus at most 8 entry moves, whatever the
 * size of the map. It does not grow with the table, which is the whole point: the old
 * behaviour was O(capacity) in one operation, and the capacity is what kept doubling.
 *
 * ## What still costs O(capacity), and why it is acceptable
 *
 * Allocating the new table clears its state array, which is one byte per slot. That is a
 * memset, not a rehash: about 8 MB at the 2^23 size that stalled for over a second, which
 * measures under a millisecond. The entry storage itself is left uninitialised, and the
 * kernel faults its pages in lazily as the migration touches them.
 *
 * ## Erasure and tombstones
 *
 * Open addressing with linear probing, and an erased slot becomes a tombstone so that probe
 * chains through it stay intact. Tombstones count towards the load factor, so a workload that
 * inserts and erases in equal measure eventually triggers a migration to the SAME capacity,
 * which clears them. Growth and tombstone-clearing are one mechanism, not two.
 *
 * ## Iterator invalidation
 *
 * Any insert or erase may move entries, so any insert or erase invalidates every iterator --
 * including the one being erased through, which is why erase(ConstIterator) does its removal
 * before it advances the migration. This is stricter than std::unordered_map, where erase
 * leaves other iterators alone, and it is the price of moving entries a few at a time.
 *
 * ## Mutating a stored value
 *
 * Iterators expose their entry as const, as tsl::robin_map's do. Use find_value(), which
 * returns a pointer straight to the stored value, or insert_or_assign(). Neither allocates.
 *
 * ## Threading
 *
 * Thread-confined, like the `tsl::robin_map` and `std::unordered_map` it stands in for, and
 * deliberately so. An application thread is the single consumer of its own message queue and
 * timer rings arrive there too, so the state it owns is touched by one thread; the reactor
 * runs its own loop and never touches application state. Rehashing on a background thread was
 * the alternative, and it would have put two threads on one table -- a lock on the hot path,
 * or a lock-free open-addressed table with tombstones, which is a hard thing to get right.
 * Migrating a few slots at a time keeps the whole structure on the owning thread instead.
 *
 * Confinement is checked rather than merely asserted in prose: under
 * PUBSUB_ITC_FW_THREAD_CHECKS the map remembers the thread that first touched it and throws
 * PreconditionAssertion if another one does.
 *
 * @tparam Key      Key type.
 * @tparam Value     Mapped type.
 * @tparam Hash      Hash function.
 * @tparam KeyEqual  Key equality predicate.
 * @tparam MigrationSlotsPerOperation Slots moved per mutating operation. The default of 8 is
 *         the tuned value; a test lowers it to 1 to drive the migration one step at a time and
 *         to reach the path where the incoming table fills before the migration ends.
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key>, size_t MigrationSlotsPerOperation = 8>
class IncrementalRehashMap {
  public:
    using EntryType = std::pair<Key, Value>;

    /// Smallest table allocated for a map that is inserted into without a reserve() first.
    static constexpr size_t minimum_capacity = 16;

    /// Slots examined per mutating operation while a migration is in flight.
    ///
    /// Two is the arithmetic floor and would leave no margin. A migration of a table of
    /// capacity C completes in C/slots operations; the table it is moving into holds C
    /// entries before it must itself grow, and the map that started the migration holds C/2,
    /// so C/2 inserts are available to finish in. Eight finishes in a quarter of that.
    static constexpr size_t migration_slots_per_operation = MigrationSlotsPerOperation;

  private:
    enum class SlotState : uint8_t { Empty = 0, Occupied = 1, Tombstone = 2 };

    /// Raw storage for one entry. A union rather than aligned_storage so that construction
    /// and destruction stay explicit, matching FixedSizeMemoryPool's slot storage.
    union EntryStorage {
        ~EntryStorage() {}
        EntryStorage() {}
        EntryStorage(const EntryStorage& other) = delete;
        EntryStorage& operator=(const EntryStorage& other) = delete;

        EntryType entry;
    };

    /// State and entry storage are separate arrays rather than one array of slot structs.
    /// The state array can then be cleared with a single memset -- it is one byte per slot of
    /// trivially copyable type -- where an array of structs holding a union would need a
    /// constructor call per slot, which is the O(capacity) cost this class exists to avoid.
    /// Probing reads the states densely, which the split also helps.
    struct Table {
        SlotState* states{nullptr};
        EntryStorage* entries{nullptr};
        size_t capacity{0};
        size_t mask{0};
        size_t live{0};
        size_t used{0};
    };

  public:
    /**
     * @brief Forward iterator over the live entries, in unspecified order.
     *
     * Walks the table being migrated out of and then the table being migrated into. An entry
     * that has already moved is Empty in the first and Occupied in the second, so a migration
     * in flight neither hides an entry nor presents one twice.
     */
    class ConstIterator {
      public:
        ConstIterator() = default;

        /**
         * @brief Constructs an iterator positioned at, or after, a given slot.
         * @param[in] owner        The map being iterated; nullptr for a default iterator.
         * @param[in] table_index  0 for the table being migrated out of, 1 for the other.
         * @param[in] slot_index   Slot to start looking from.
         */
        ConstIterator(const IncrementalRehashMap* owner, size_t table_index, size_t slot_index)
            : owner_(owner), table_index_(table_index), slot_index_(slot_index) {
            advance_to_occupied();
        }

        [[nodiscard]] const EntryType& operator*() const {
            return owner_->table_for(table_index_).entries[slot_index_].entry;
        }

        [[nodiscard]] const EntryType* operator->() const {
            return &owner_->table_for(table_index_).entries[slot_index_].entry;
        }

        ConstIterator& operator++() {
            ++slot_index_;
            advance_to_occupied();
            return *this;
        }

        [[nodiscard]] bool operator==(const ConstIterator& other) const {
            return owner_ == other.owner_ && table_index_ == other.table_index_ && slot_index_ == other.slot_index_;
        }

        [[nodiscard]] bool operator!=(const ConstIterator& other) const {
            return !(*this == other);
        }

      private:
        friend class IncrementalRehashMap;

        /// Moves to the next occupied slot, crossing into the second table and finally
        /// landing on the end position, which is slot 0 of a notional third table.
        void advance_to_occupied() {
            while (owner_ != nullptr && table_index_ < 2) {
                const Table& table = owner_->table_for(table_index_);
                while (slot_index_ < table.capacity) {
                    if (table.states[slot_index_] == SlotState::Occupied) {
                        return;
                    }
                    ++slot_index_;
                }
                ++table_index_;
                slot_index_ = 0;
            }
            table_index_ = 2;
            slot_index_ = 0;
        }

        const IncrementalRehashMap* owner_{nullptr};
        size_t table_index_{0};
        size_t slot_index_{0};
    };

    ~IncrementalRehashMap() {
        release_table(current_);
        release_table(next_);
    }

    /**
     * @brief Constructs an empty map, allocating nothing.
     * @param[in] initial_capacity Entries to make room for, or 0 to allocate on first insert.
     * @param[in] hash             Hash function.
     * @param[in] key_equal        Key equality predicate.
     * @param[in] growth_reporter  Non-owning, may be nullptr. Told the size of each table this
     *                             map allocates, so that a structure growing without bound is
     *                             visible before the OOM killer finds it.
     */
    explicit IncrementalRehashMap(size_t initial_capacity = 0, const Hash& hash = Hash{}, const KeyEqual& key_equal = KeyEqual{},
                                  AllocationGrowthReporter* growth_reporter = nullptr)
        : hash_(hash), key_equal_(key_equal), growth_reporter_(growth_reporter) {
        if (initial_capacity > 0) {
            reserve(initial_capacity);
        }
    }

    /// Copying would duplicate two raw tables and an in-flight migration, and no caller has
    /// ever wanted it. Moving is supported: the tables transfer as they stand, migration
    /// included, which is why the cursor moves with them.
    IncrementalRehashMap(const IncrementalRehashMap& other) = delete;
    IncrementalRehashMap& operator=(const IncrementalRehashMap& other) = delete;

    IncrementalRehashMap(IncrementalRehashMap&& other)
        : hash_(std::move(other.hash_))
        , key_equal_(std::move(other.key_equal_))
        , growth_reporter_(other.growth_reporter_)
        , current_(other.current_)
        , next_(other.next_)
        , migration_cursor_(other.migration_cursor_)
        , size_(other.size_) {
        other.current_ = Table{};
        other.next_ = Table{};
        other.migration_cursor_ = 0;
        other.size_ = 0;
    }

    IncrementalRehashMap& operator=(IncrementalRehashMap&& other) {
        if (this == &other) {
            return *this;
        }
        release_table(current_);
        release_table(next_);
        hash_ = std::move(other.hash_);
        key_equal_ = std::move(other.key_equal_);
        growth_reporter_ = other.growth_reporter_;
        current_ = other.current_;
        next_ = other.next_;
        migration_cursor_ = other.migration_cursor_;
        size_ = other.size_;
        other.current_ = Table{};
        other.next_ = Table{};
        other.migration_cursor_ = 0;
        other.size_ = 0;
        return *this;
    }

    [[nodiscard]] size_t size() const {
        return size_;
    }

    [[nodiscard]] bool empty() const {
        return size_ == 0;
    }

    /// Slots across both tables. Larger than the number of entries by design: the load factor
    /// is held at a half, above which linear probing degrades sharply.
    [[nodiscard]] size_t capacity() const {
        return current_.capacity + next_.capacity;
    }

    /// True while entries are being moved between tables a few at a time.
    [[nodiscard]] bool is_migrating() const {
        return next_.capacity > 0;
    }

    /// Slots of the outgoing table not yet examined by the migration. Zero when settled.
    [[nodiscard]] size_t migration_slots_remaining() const {
        return is_migrating() ? current_.capacity - migration_cursor_ : 0;
    }

    [[nodiscard]] ConstIterator begin() const {
        check_thread_confinement();
        return ConstIterator(this, 0, 0);
    }

    [[nodiscard]] ConstIterator end() const {
        return ConstIterator(this, 2, 0);
    }

    /**
     * @brief Makes room for @p entry_count entries without a migration, where it can.
     * @param[in] entry_count Entries the caller expects to hold.
     *
     * On an empty map this allocates the table outright, which is the cheap way to size a
     * structure whose eventual size is known -- one allocation, no migration, no growth.
     * On a map that already holds entries it starts a migration to the new size, so even a
     * late reserve() does not stall the caller.
     */
    void reserve(size_t entry_count) {
        check_thread_confinement();
        const size_t wanted = capacity_for(entry_count);
        if (size_ == 0 && !is_migrating()) {
            if (wanted > current_.capacity) {
                Table replacement = allocate_table(wanted);
                release_table(current_);
                current_ = replacement;
            }
            return;
        }
        if (wanted > current_.capacity && !is_migrating()) {
            begin_migration(wanted);
        }
    }

    [[nodiscard]] size_t count(const Key& key) const {
        return find(key) == end() ? 0 : 1;
    }

    /**
     * @brief Finds an entry.
     * @param[in] key The key to look for.
     * @return An iterator to the entry, or end() when there is none.
     */
    [[nodiscard]] ConstIterator find(const Key& key) const {
        check_thread_confinement();
        const size_t hash_value = hash_(key);
        if (is_migrating()) {
            const size_t slot = find_slot(next_, key, hash_value);
            if (slot != next_.capacity) {
                return ConstIterator(this, 1, slot);
            }
        }
        const size_t slot = find_slot(current_, key, hash_value);
        if (slot != current_.capacity) {
            return ConstIterator(this, 0, slot);
        }
        return end();
    }

    /**
     * @brief Finds an entry's value for modification in place.
     * @param[in] key The key to look for.
     * @return A pointer to the stored value, or nullptr when there is no such entry.
     *
     * The mutable counterpart to find(), whose iterators are const. Nothing is allocated and
     * nothing is moved, so this is the cheap way to update a value that is already present.
     */
    [[nodiscard]] Value* find_value(const Key& key) {
        check_thread_confinement();
        const size_t hash_value = hash_(key);
        if (is_migrating()) {
            const size_t slot = find_slot(next_, key, hash_value);
            if (slot != next_.capacity) {
                return &next_.entries[slot].entry.second;
            }
        }
        const size_t slot = find_slot(current_, key, hash_value);
        if (slot != current_.capacity) {
            return &current_.entries[slot].entry.second;
        }
        return nullptr;
    }

    /**
     * @brief Inserts an entry if the key is not already present.
     * @param[in] key   The key.
     * @param[in] value The value, copied.
     * @return The entry and true when inserted; the existing entry and false when not.
     */
    std::pair<ConstIterator, bool> emplace(const Key& key, const Value& value) {
        return insert_entry(key, value, AssignExisting::No);
    }

    /**
     * @brief Inserts an entry, overwriting the value if the key is already present.
     * @param[in] key   The key.
     * @param[in] value The value, copied.
     * @return The entry and true when inserted; the entry and false when it was overwritten.
     */
    std::pair<ConstIterator, bool> insert_or_assign(const Key& key, const Value& value) {
        return insert_entry(key, value, AssignExisting::Yes);
    }

    /**
     * @brief Erases the entry with this key, if there is one.
     * @param[in] key The key.
     * @return 1 when an entry was erased, 0 when there was none.
     */
    size_t erase(const Key& key) {
        check_thread_confinement();
        const ConstIterator position = find(key);
        if (position == end()) {
            // A failed erase still advances the migration. It is a mutating call by intent,
            // and a workload of mostly-missing erases must not leave a migration stranded.
            step_migration();
            return 0;
        }
        erase(position);
        return 1;
    }

    /**
     * @brief Erases the entry an iterator refers to.
     * @param[in] position The entry to erase; end() is ignored.
     * @return An iterator to the following entry.
     *
     * Every iterator is invalidated, this one included. The returned iterator is computed
     * before the migration is advanced and is safe to use; anything else held across the call
     * is not.
     */
    ConstIterator erase(const ConstIterator& position) {
        check_thread_confinement();
        if (position == end()) {
            return end();
        }
        // Removal happens before the migration step, because the step may move the very entry
        // this iterator names.
        Table& table = mutable_table_for(position.table_index_);
        table.entries[position.slot_index_].entry.~EntryType();
        table.states[position.slot_index_] = SlotState::Tombstone;
        --table.live;
        --size_;

        ConstIterator following(this, position.table_index_, position.slot_index_ + 1);
        step_migration();
        return following;
    }

    /// Destroys every entry, keeping the current table's storage so that a map that is
    /// cleared and refilled does not have to grow its way back.
    void clear() {
        check_thread_confinement();
        destroy_entries(current_);
        release_table(next_);
        next_ = Table{};
        migration_cursor_ = 0;
        size_ = 0;
    }

  private:
    enum class AssignExisting : uint8_t { No = 0, Yes = 1 };

    /// Records the thread that first touches the map and rejects any other.
    ///
    /// Compiled to nothing unless PUBSUB_ITC_FW_THREAD_CHECKS is set, so the release hot path
    /// is unchanged. Thread confinement here is a property of the caller rather than of this
    /// class -- the map has no synchronisation and is not trying to acquire any -- so the
    /// useful thing it can do is refuse to let a second thread in unnoticed. The container it
    /// replaces offers no such check: a second thread on a robin_map is silent corruption.
    ///
    /// const, and the recorded id is mutable, because a lookup establishes ownership just as
    /// an insert does. A reader on the wrong thread races an insert exactly as another writer
    /// would.
    void check_thread_confinement() const {
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
        const std::thread::id calling_thread = std::this_thread::get_id();
        if (owning_thread_ == std::thread::id{}) {
            owning_thread_ = calling_thread;
            return;
        }
        if (owning_thread_ != calling_thread) {
            throw PreconditionAssertion("IncrementalRehashMap is thread-confined and was touched by a second thread", __FILE__, __LINE__);
        }
#endif
    }

    [[nodiscard]] const Table& table_for(size_t table_index) const {
        return table_index == 0 ? current_ : next_;
    }

    [[nodiscard]] Table& mutable_table_for(size_t table_index) {
        return table_index == 0 ? current_ : next_;
    }

    /// Smallest power of two whose half is at least @p entry_count, the load factor being a
    /// half. A power of two so that the modulo is a mask.
    [[nodiscard]] static size_t capacity_for(size_t entry_count) {
        size_t wanted = minimum_capacity;
        while (wanted / 2 < entry_count) {
            wanted *= 2;
        }
        return wanted;
    }

    /**
     * @brief Tells the reporter how large this map's newest table is.
     *
     * The whole table in one report, both arrays together, which is what an operator wants to
     * know: a map reaching a gigabyte does so as one structure, not as a stream of unrelated
     * allocations. Reported before the memory is used, so the last report before an exhausted
     * machine names the size that could not be sustained.
     */
    void report_table_allocation(size_t bytes) {
        if (growth_reporter_ == nullptr || bytes < growth_reporter_->report_threshold_bytes) {
            return;
        }
        size_t previous = growth_reporter_->largest_allocation_bytes.load(std::memory_order_relaxed);
        while (bytes > previous && !growth_reporter_->largest_allocation_bytes.compare_exchange_weak(previous, bytes, std::memory_order_relaxed)) {}
        if (growth_reporter_->on_large_allocation) {
            growth_reporter_->on_large_allocation(bytes, growth_reporter_->largest_allocation_bytes.load(std::memory_order_relaxed));
        }
    }

    /**
     * @brief Allocates a table of @p table_capacity slots and reports its size.
     *
     * Raw storage from the global operator new rather than a std::allocator. The map
     * constructs and destroys entries itself, in place, and a container that placement-news
     * into memory an allocator handed it is only half inside the allocator model anyway:
     * a std::allocator_traits<A>::construct that the allocator meant to observe is skipped.
     * Owning the memory outright makes the placement new and the explicit destructor calls
     * unambiguously correct.
     *
     * The aligned form is used for the entry array because Value may be over-aligned -- a
     * cache-line-aligned value is an ordinary thing to hold in a venue -- and the plain
     * operator new only promises alignment up to max_align_t.
     */
    [[nodiscard]] Table allocate_table(size_t table_capacity) {
        const size_t state_bytes = table_capacity * sizeof(SlotState);
        const size_t entry_bytes = table_capacity * sizeof(EntryStorage);

        Table table;
        table.states = static_cast<SlotState*>(::operator new(state_bytes));
        table.entries = static_cast<EntryStorage*>(::operator new(entry_bytes, std::align_val_t{alignof(EntryStorage)}));
        table.capacity = table_capacity;
        table.mask = table_capacity - 1;
        report_table_allocation(state_bytes + entry_bytes);
        // One byte per slot, and SlotState::Empty is zero. This is the only part of allocating
        // a table that touches every slot; the entry storage is left raw and is faulted in by
        // the migration as it goes.
        std::memset(table.states, 0, table_capacity * sizeof(SlotState));
        return table;
    }

    void destroy_entries(Table& table) {
        if (table.states == nullptr) {
            return;
        }
        for (size_t slot = 0; slot < table.capacity; ++slot) {
            if (table.states[slot] == SlotState::Occupied) {
                table.entries[slot].entry.~EntryType();
            }
        }
        std::memset(table.states, 0, table.capacity * sizeof(SlotState));
        table.live = 0;
        table.used = 0;
    }

    void release_table(Table& table) {
        if (table.states == nullptr) {
            return;
        }
        destroy_entries(table);
        ::operator delete(table.states);
        ::operator delete(table.entries, std::align_val_t{alignof(EntryStorage)});
        table = Table{};
    }

    /// Index of the slot holding @p key, or table.capacity when it is not there. Stops at the
    /// first Empty slot: a tombstone is skipped, because the entry sought may have been
    /// inserted beyond it before whatever was here was erased.
    [[nodiscard]] size_t find_slot(const Table& table, const Key& key, size_t hash_value) const {
        if (table.capacity == 0) {
            return 0;
        }
        size_t slot = hash_value & table.mask;
        for (size_t probe = 0; probe < table.capacity; ++probe) {
            const SlotState state = table.states[slot];
            if (state == SlotState::Empty) {
                return table.capacity;
            }
            if (state == SlotState::Occupied && key_equal_(table.entries[slot].entry.first, key)) {
                return slot;
            }
            slot = (slot + 1) & table.mask;
        }
        // Not reachable while the load factor holds: a table never exceeds half full counting
        // tombstones, so some slot is always Empty and the loop always leaves through the
        // return above. It is a bound rather than a `while (true)`, so that a future change
        // which broke that invariant would give a failed lookup rather than a hung thread.
        // It shows as the one uncovered line in this file, and no test can honestly reach it.
        return table.capacity;
    }

    /// Index of the slot a new entry belongs in. The caller must have established that the
    /// key is absent, so the first slot that is not occupied will do.
    [[nodiscard]] size_t find_free_slot(const Table& table, size_t hash_value) const {
        size_t slot = hash_value & table.mask;
        while (table.states[slot] == SlotState::Occupied) {
            slot = (slot + 1) & table.mask;
        }
        return slot;
    }

    void construct_at(Table& table, size_t slot, const Key& key, const Value& value) {
        const SlotState previous = table.states[slot];
        new (&table.entries[slot].entry) EntryType(key, value);
        table.states[slot] = SlotState::Occupied;
        ++table.live;
        // A tombstone already counted towards the load: reusing it does not add to the count,
        // and counting it twice would migrate a table that is not filling up.
        if (previous == SlotState::Empty) {
            ++table.used;
        }
    }

    std::pair<ConstIterator, bool> insert_entry(const Key& key, const Value& value, AssignExisting assign_existing) {
        check_thread_confinement();
        step_migration();

        const size_t hash_value = hash_(key);
        if (is_migrating()) {
            const size_t existing = find_slot(next_, key, hash_value);
            if (existing != next_.capacity) {
                if (assign_existing == AssignExisting::Yes) {
                    next_.entries[existing].entry.second = value;
                }
                return {ConstIterator(this, 1, existing), false};
            }
        }
        const size_t existing = find_slot(current_, key, hash_value);
        if (existing != current_.capacity) {
            if (assign_existing == AssignExisting::Yes) {
                current_.entries[existing].entry.second = value;
            }
            return {ConstIterator(this, 0, existing), false};
        }

        make_room_for_one();

        Table& target = is_migrating() ? next_ : current_;
        const size_t table_index = is_migrating() ? 1 : 0;
        const size_t slot = find_free_slot(target, hash_value);
        construct_at(target, slot, key, value);
        ++size_;
        return {ConstIterator(this, table_index, slot), true};
    }

    /// Ensures the table an insert is about to go into has room, starting a migration if not.
    void make_room_for_one() {
        if (current_.capacity == 0 && !is_migrating()) {
            current_ = allocate_table(minimum_capacity);
            return;
        }

        Table& target = is_migrating() ? next_ : current_;
        if ((target.used + 1) * 2 <= target.capacity) {
            return;
        }

        if (is_migrating()) {
            // The table being migrated into has filled before the migration finished. With
            // eight slots per operation that needs a workload this class is not tuned for, so
            // finish the move rather than carry a third table, and let the ordinary path start
            // the next migration from a settled state.
            complete_migration();
        }

        // Doubling when the entries themselves fill the table, same size when it is tombstones
        // that fill it -- an insert/erase workload in balance reclaims its slots without ever
        // growing.
        const size_t target_capacity = current_.live * 2 * 2 > current_.capacity ? current_.capacity * 2 : current_.capacity;
        begin_migration(target_capacity);
    }

    void begin_migration(size_t target_capacity) {
        next_ = allocate_table(target_capacity);
        migration_cursor_ = 0;
    }

    /// Moves at most migration_slots_per_operation slots' worth of entries across. This is the
    /// bounded share of the growth cost that each mutating operation pays.
    void step_migration() {
        if (!is_migrating()) {
            return;
        }
        size_t examined = 0;
        while (migration_cursor_ < current_.capacity && examined < migration_slots_per_operation) {
            move_slot(migration_cursor_);
            ++migration_cursor_;
            ++examined;
        }
        if (migration_cursor_ >= current_.capacity) {
            finish_migration();
        }
    }

    void complete_migration() {
        while (is_migrating()) {
            step_migration();
        }
    }

    void move_slot(size_t slot) {
        if (current_.states[slot] != SlotState::Occupied) {
            return;
        }
        EntryType& entry = current_.entries[slot].entry;
        const size_t destination = find_free_slot(next_, hash_(entry.first));
        new (&next_.entries[destination].entry) EntryType(std::move(entry));
        next_.states[destination] = SlotState::Occupied;
        ++next_.live;
        ++next_.used;

        entry.~EntryType();
        current_.states[slot] = SlotState::Empty;
        --current_.live;
    }

    void finish_migration() {
        release_table(current_);
        current_ = next_;
        next_ = Table{};
        migration_cursor_ = 0;
    }

    Hash hash_;
    KeyEqual key_equal_;
    AllocationGrowthReporter* growth_reporter_{nullptr};

    Table current_;
    Table next_;
    size_t migration_cursor_{0};
    size_t size_{0};

#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
    mutable std::thread::id owning_thread_{};
#endif
};

} // namespaces
