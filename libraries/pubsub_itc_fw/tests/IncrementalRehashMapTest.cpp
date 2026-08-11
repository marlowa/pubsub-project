// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/GrowthReportingAllocator.hpp> // IWYU pragma: keep -- AllocationGrowthReporter
#include <pubsub_itc_fw/IncrementalRehashMap.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw::tests {

namespace {

/**
 * @brief A value type that counts every construction, move and destruction.
 *
 * The map holds raw storage and constructs and destroys entries by hand, so "every entry that
 * was constructed was destroyed exactly once" is a property that has to be measured rather
 * than assumed. The same counters answer the question the whole class exists for: how many
 * entries did one operation move?
 */
struct CountedValue {
    static int constructions;
    static int destructions;
    static int moves;

    static void reset_counts() {
        constructions = 0;
        destructions = 0;
        moves = 0;
    }

    static int live_count() {
        return constructions - destructions;
    }

    ~CountedValue() {
        ++destructions;
    }

    CountedValue() {
        ++constructions;
    }

    explicit CountedValue(int payload_value) : payload(payload_value) {
        ++constructions;
    }

    CountedValue(const CountedValue& other) : payload(other.payload) {
        ++constructions;
    }

    CountedValue(CountedValue&& other) : payload(other.payload) {
        ++constructions;
        ++moves;
    }

    CountedValue& operator=(const CountedValue& other) {
        payload = other.payload;
        return *this;
    }

    CountedValue& operator=(CountedValue&& other) {
        payload = other.payload;
        ++moves;
        return *this;
    }

    int payload{0};
};

int CountedValue::constructions = 0;
int CountedValue::destructions = 0;
int CountedValue::moves = 0;

/// Every key lands in one bucket, so every probe chain is as long as the map is full. The
/// worst case for open addressing, and the case where a mishandled tombstone shows up.
struct ConstantHash {
    size_t operator()(int) const {
        return 0;
    }
};

/// A handful of buckets, so keys collide heavily without collapsing to a single chain.
struct NarrowHash {
    size_t operator()(int key) const {
        return static_cast<size_t>(key) & 0x3U;
    }
};

using IntMap = IncrementalRehashMap<int, int>;
using StringMap = IncrementalRehashMap<int, std::string>;
using CountedMap = IncrementalRehashMap<int, CountedValue>;

/// Migration rate of one slot per operation: the migration then advances in steps a test can
/// count, and the incoming table can be made to fill before the migration finishes.
using SteppedMap = IncrementalRehashMap<int, int, std::hash<int>, std::equal_to<int>, 1>;
using SteppedCountedMap = IncrementalRehashMap<int, CountedValue, std::hash<int>, std::equal_to<int>, 1>;

using ConstantHashMap = IncrementalRehashMap<int, int, ConstantHash>;
using NarrowHashMap = IncrementalRehashMap<int, int, NarrowHash>;

/// Fills a map until a migration is in flight, returning the number of entries inserted.
template <typename MapType> int fill_until_migrating(MapType& map) {
    int key = 0;
    while (!map.is_migrating() && key < 100000) {
        map.emplace(key, key);
        ++key;
    }
    return key;
}

} // namespaces

class IncrementalRehashMapTest : public ::testing::Test {
  protected:
    void SetUp() override {
        CountedValue::reset_counts();
    }
};

TEST_F(IncrementalRehashMapTest, EmptyMapHasNoEntriesAndAllocatesNothing) {
    IntMap map;
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.capacity(), 0u);
    EXPECT_FALSE(map.is_migrating());
    EXPECT_EQ(map.begin(), map.end());
}

TEST_F(IncrementalRehashMapTest, InsertThenFindReturnsTheEntry) {
    IntMap map;
    const auto result = map.emplace(42, 4200);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(result.first->first, 42);
    EXPECT_EQ(result.first->second, 4200);
    EXPECT_EQ(map.size(), 1u);

    const auto found = map.find(42);
    ASSERT_NE(found, map.end());
    EXPECT_EQ(found->second, 4200);
    EXPECT_EQ(map.count(42), 1u);
}

TEST_F(IncrementalRehashMapTest, FindMissingKeyReturnsEnd) {
    IntMap map;
    map.emplace(1, 1);
    EXPECT_EQ(map.find(2), map.end());
    EXPECT_EQ(map.count(2), 0u);
}

TEST_F(IncrementalRehashMapTest, FindOnAnEmptyMapReturnsEnd) {
    IntMap map;
    EXPECT_EQ(map.find(7), map.end());
    EXPECT_EQ(map.find_value(7), nullptr);
}

TEST_F(IncrementalRehashMapTest, EmplaceLeavesAnExistingEntryAlone) {
    IntMap map;
    map.emplace(1, 100);
    const auto result = map.emplace(1, 999);
    EXPECT_FALSE(result.second);
    EXPECT_EQ(result.first->second, 100);
    EXPECT_EQ(map.size(), 1u);
}

TEST_F(IncrementalRehashMapTest, InsertOrAssignOverwritesAnExistingEntry) {
    IntMap map;
    map.emplace(1, 100);
    const auto result = map.insert_or_assign(1, 999);
    EXPECT_FALSE(result.second);
    EXPECT_EQ(result.first->second, 999);
    EXPECT_EQ(map.size(), 1u);
    EXPECT_EQ(map.find(1)->second, 999);
}

TEST_F(IncrementalRehashMapTest, InsertOrAssignInsertsWhenAbsent) {
    IntMap map;
    const auto result = map.insert_or_assign(1, 100);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(map.size(), 1u);
}

TEST_F(IncrementalRehashMapTest, FindValueGivesAPointerForModificationInPlace) {
    IntMap map;
    map.emplace(1, 100);
    int* value = map.find_value(1);
    ASSERT_NE(value, nullptr);
    *value = 555;
    EXPECT_EQ(map.find(1)->second, 555);
    EXPECT_EQ(map.find_value(2), nullptr);
}

TEST_F(IncrementalRehashMapTest, EraseRemovesTheEntry) {
    IntMap map;
    map.emplace(1, 100);
    EXPECT_EQ(map.erase(1), 1u);
    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.find(1), map.end());
}

TEST_F(IncrementalRehashMapTest, EraseOfAMissingKeyReportsNothingRemoved) {
    IntMap map;
    map.emplace(1, 100);
    EXPECT_EQ(map.erase(2), 0u);
    EXPECT_EQ(map.size(), 1u);
}

TEST_F(IncrementalRehashMapTest, EraseByIteratorReturnsTheFollowingEntry) {
    IntMap map;
    for (int key = 0; key < 5; ++key) {
        map.emplace(key, key);
    }
    const auto position = map.find(3);
    ASSERT_NE(position, map.end());
    const auto following = map.erase(position);
    EXPECT_EQ(map.size(), 4u);
    EXPECT_EQ(map.find(3), map.end());
    // The order is unspecified, so the guarantee under test is that the returned iterator is
    // usable and never names the erased entry.
    if (following != map.end()) {
        EXPECT_NE(following->first, 3);
    }
}

TEST_F(IncrementalRehashMapTest, ErasingEndIsIgnored) {
    IntMap map;
    map.emplace(1, 100);
    EXPECT_EQ(map.erase(map.end()), map.end());
    EXPECT_EQ(map.size(), 1u);
}

TEST_F(IncrementalRehashMapTest, AnErasedSlotIsReusedByTheNextInsert) {
    IntMap map;
    map.reserve(8);
    const size_t capacity_before = map.capacity();
    for (int round = 0; round < 1000; ++round) {
        map.emplace(round, round);
        EXPECT_EQ(map.erase(round), 1u);
    }
    EXPECT_EQ(map.size(), 0u);
    // 1000 inserts into a table of 16 slots, each erased again. Tombstones are reclaimed by a
    // same-size migration, so the capacity must not have run away.
    EXPECT_LE(map.capacity(), capacity_before * 4);
}

TEST_F(IncrementalRehashMapTest, ClearEmptiesTheMapAndKeepsItsCapacity) {
    IntMap map;
    map.reserve(1000);
    const size_t capacity_before = map.capacity();
    for (int key = 0; key < 500; ++key) {
        map.emplace(key, key);
    }
    map.clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.begin(), map.end());
    EXPECT_EQ(map.capacity(), capacity_before);
    EXPECT_FALSE(map.is_migrating());

    map.emplace(1, 1);
    EXPECT_EQ(map.find(1)->second, 1);
}

TEST_F(IncrementalRehashMapTest, ReserveOnAnEmptyMapAllocatesWithoutMigrating) {
    IntMap map;
    map.reserve(1000);
    EXPECT_GE(map.capacity(), 2000u); // load factor of a half
    EXPECT_FALSE(map.is_migrating());

    for (int key = 0; key < 1000; ++key) {
        map.emplace(key, key);
    }
    // The whole point of pre-sizing: a thousand entries went in without a single migration.
    EXPECT_FALSE(map.is_migrating());
    EXPECT_EQ(map.size(), 1000u);
}

TEST_F(IncrementalRehashMapTest, ReserveOnAPopulatedMapMigratesRatherThanStalling) {
    IntMap map;
    for (int key = 0; key < 100; ++key) {
        map.emplace(key, key);
    }
    map.reserve(100000);
    EXPECT_TRUE(map.is_migrating());

    for (int key = 0; key < 100; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost by a reserve-driven migration";
    }
}

TEST_F(IncrementalRehashMapTest, IterationVisitsEveryEntryExactlyOnce) {
    IntMap map;
    for (int key = 0; key < 500; ++key) {
        map.emplace(key, key * 2);
    }
    std::vector<int> seen(500, 0);
    size_t visited = 0;
    for (const auto& entry : map) {
        ASSERT_GE(entry.first, 0);
        ASSERT_LT(entry.first, 500);
        EXPECT_EQ(entry.second, entry.first * 2);
        ++seen[static_cast<size_t>(entry.first)];
        ++visited;
    }
    EXPECT_EQ(visited, 500u);
    for (size_t index = 0; index < seen.size(); ++index) {
        EXPECT_EQ(seen[index], 1) << "entry " << index << " visited " << seen[index] << " times";
    }
}

TEST_F(IncrementalRehashMapTest, MoveConstructionTransfersTheEntries) {
    IntMap source;
    for (int key = 0; key < 100; ++key) {
        source.emplace(key, key);
    }
    IntMap moved(std::move(source));
    EXPECT_EQ(moved.size(), 100u);
    EXPECT_EQ(moved.find(50)->second, 50);
}

TEST_F(IncrementalRehashMapTest, MoveAssignmentReleasesWhatTheTargetHeld) {
    IntMap source;
    for (int key = 0; key < 100; ++key) {
        source.emplace(key, key);
    }
    IntMap target;
    for (int key = 0; key < 10; ++key) {
        target.emplace(key + 1000, key);
    }
    target = std::move(source);
    EXPECT_EQ(target.size(), 100u);
    EXPECT_EQ(target.find(50)->second, 50);
    EXPECT_EQ(target.find(1000), target.end());
}

TEST_F(IncrementalRehashMapTest, GrowthStartsAMigrationRatherThanRehashingInOneGo) {
    SteppedMap map;
    const int inserted = fill_until_migrating(map);
    EXPECT_TRUE(map.is_migrating());
    EXPECT_GT(map.migration_slots_remaining(), 0u);
    for (int key = 0; key < inserted; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost at the start of a migration";
    }
}

TEST_F(IncrementalRehashMapTest, AMigrationFinishesWithinTheOperationsItsRateAllows) {
    SteppedMap map;
    fill_until_migrating(map);
    ASSERT_TRUE(map.is_migrating());

    // One slot per operation, so the migration must be over within the number of slots it has
    // left. Erases of absent keys are used as the driver: they mutate nothing else.
    const size_t remaining = map.migration_slots_remaining();
    for (size_t step = 0; step < remaining; ++step) {
        map.erase(-1);
    }
    EXPECT_FALSE(map.is_migrating());
    EXPECT_EQ(map.migration_slots_remaining(), 0u);
}

TEST_F(IncrementalRehashMapTest, EntriesAreFoundInWhicheverTableHoldsThem) {
    SteppedMap map;
    const int inserted = fill_until_migrating(map);
    ASSERT_TRUE(map.is_migrating());

    // Half way through the move, some entries are in the outgoing table and some in the
    // incoming one. Every one of them must still be found.
    const size_t half = map.migration_slots_remaining() / 2;
    for (size_t step = 0; step < half; ++step) {
        map.erase(-1);
    }
    ASSERT_TRUE(map.is_migrating());
    for (int key = 0; key < inserted; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost mid-migration";
        EXPECT_EQ(map.find(key)->second, key);
    }
}

TEST_F(IncrementalRehashMapTest, EraseMidMigrationRemovesFromEitherTable) {
    SteppedMap map;
    const int inserted = fill_until_migrating(map);
    ASSERT_TRUE(map.is_migrating());

    size_t expected_size = static_cast<size_t>(inserted);
    for (int key = 0; key < inserted; key += 2) {
        EXPECT_EQ(map.erase(key), 1u) << "entry " << key << " could not be erased mid-migration";
        --expected_size;
        EXPECT_EQ(map.size(), expected_size);
    }
    for (int key = 0; key < inserted; ++key) {
        const bool should_be_present = (key % 2) == 1;
        EXPECT_EQ(map.find(key) != map.end(), should_be_present) << "entry " << key;
    }
}

TEST_F(IncrementalRehashMapTest, IterationMidMigrationVisitsEveryEntryExactlyOnce) {
    SteppedMap map;
    const int inserted = fill_until_migrating(map);
    ASSERT_TRUE(map.is_migrating());

    // Step part way through so that both tables hold entries.
    const size_t quarter = map.migration_slots_remaining() / 4;
    for (size_t step = 0; step < quarter; ++step) {
        map.erase(-1);
    }
    ASSERT_TRUE(map.is_migrating());

    std::vector<int> seen(static_cast<size_t>(inserted), 0);
    size_t visited = 0;
    for (const auto& entry : map) {
        ++seen[static_cast<size_t>(entry.first)];
        ++visited;
    }
    EXPECT_EQ(visited, static_cast<size_t>(inserted));
    for (size_t index = 0; index < seen.size(); ++index) {
        EXPECT_EQ(seen[index], 1) << "entry " << index << " visited " << seen[index] << " times mid-migration";
    }
}

TEST_F(IncrementalRehashMapTest, InsertMidMigrationIsFoundAndCounted) {
    SteppedMap map;
    const int inserted = fill_until_migrating(map);
    ASSERT_TRUE(map.is_migrating());

    const auto result = map.emplace(inserted + 1, 12345);
    EXPECT_TRUE(result.second);
    EXPECT_EQ(map.find(inserted + 1)->second, 12345);
    EXPECT_EQ(map.size(), static_cast<size_t>(inserted) + 1);
}

TEST_F(IncrementalRehashMapTest, TheIncomingTableFillingMidMigrationIsHandled) {
    // The fallback path: at one slot per operation the migration cannot keep up with a stream
    // of inserts, so the incoming table fills before the move is done and the map must finish
    // the migration itself rather than carry a third table. Unreachable at the default rate,
    // which is why the rate is a template parameter.
    SteppedMap map;
    for (int key = 0; key < 20000; ++key) {
        map.emplace(key, key);
    }
    EXPECT_EQ(map.size(), 20000u);
    for (int key = 0; key < 20000; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost when the incoming table filled";
        EXPECT_EQ(map.find(key)->second, key);
    }
}

TEST_F(IncrementalRehashMapTest, ConstantHashFindsEveryEntry) {
    ConstantHashMap map;
    for (int key = 0; key < 500; ++key) {
        map.emplace(key, key * 3);
    }
    EXPECT_EQ(map.size(), 500u);
    for (int key = 0; key < 500; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost in a single probe chain";
        EXPECT_EQ(map.find(key)->second, key * 3);
    }
    EXPECT_EQ(map.find(500), map.end());
}

TEST_F(IncrementalRehashMapTest, ConstantHashSurvivesErasureFromTheMiddleOfTheChain) {
    ConstantHashMap map;
    for (int key = 0; key < 200; ++key) {
        map.emplace(key, key);
    }
    // Erase from the middle of one long probe chain. A tombstone that terminated a probe
    // instead of being skipped would lose every entry beyond it.
    for (int key = 50; key < 150; ++key) {
        EXPECT_EQ(map.erase(key), 1u);
    }
    for (int key = 0; key < 50; ++key) {
        EXPECT_NE(map.find(key), map.end()) << "entry " << key << " lost from before the tombstones";
    }
    for (int key = 150; key < 200; ++key) {
        EXPECT_NE(map.find(key), map.end()) << "entry " << key << " lost from beyond the tombstones";
    }
    for (int key = 50; key < 150; ++key) {
        EXPECT_EQ(map.find(key), map.end()) << "erased entry " << key << " still present";
    }
}

TEST_F(IncrementalRehashMapTest, NarrowHashClustersWithoutLosingEntries) {
    NarrowHashMap map;
    for (int key = 0; key < 1000; ++key) {
        map.emplace(key, key);
    }
    for (int key = 0; key < 1000; ++key) {
        ASSERT_NE(map.find(key), map.end()) << "entry " << key << " lost under heavy clustering";
    }
    for (int key = 0; key < 1000; key += 3) {
        EXPECT_EQ(map.erase(key), 1u);
    }
    for (int key = 0; key < 1000; ++key) {
        const bool should_be_present = (key % 3) != 0;
        EXPECT_EQ(map.find(key) != map.end(), should_be_present) << "entry " << key;
    }
}

TEST_F(IncrementalRehashMapTest, DestructionDestroysEveryEntry) {
    {
        CountedMap map;
        for (int key = 0; key < 1000; ++key) {
            map.emplace(key, CountedValue(key));
        }
        EXPECT_GT(CountedValue::live_count(), 0);
    }
    EXPECT_EQ(CountedValue::live_count(), 0) << "the map's destructor left entries alive";
}

TEST_F(IncrementalRehashMapTest, ClearDestroysEveryEntry) {
    CountedMap map;
    for (int key = 0; key < 500; ++key) {
        map.emplace(key, CountedValue(key));
    }
    map.clear();
    EXPECT_EQ(CountedValue::live_count(), 0) << "clear() left entries alive";
}

TEST_F(IncrementalRehashMapTest, EraseDestroysTheEntryItRemoves) {
    CountedMap map;
    map.emplace(1, CountedValue(1));
    const int live_before = CountedValue::live_count();
    EXPECT_EQ(map.erase(1), 1u);
    EXPECT_EQ(CountedValue::live_count(), live_before - 1);
}

TEST_F(IncrementalRehashMapTest, MigrationDestroysTheEntryItMovedFrom) {
    SteppedCountedMap map;
    int key = 0;
    while (!map.is_migrating() && key < 100000) {
        map.emplace(key, CountedValue(key));
        ++key;
    }
    ASSERT_TRUE(map.is_migrating());

    // Drive the migration to completion, then check that a moved-from entry was destroyed
    // rather than merely abandoned: one live value per entry in the map, and no more.
    while (map.is_migrating()) {
        map.erase(-1);
    }
    EXPECT_EQ(CountedValue::live_count(), key) << "a migration left moved-from entries alive";
}

TEST_F(IncrementalRehashMapTest, NoSingleOperationMovesMoreThanTheMigrationRate) {
    // The property the class exists for. Counted rather than timed, so a loaded machine cannot
    // make it flaky, and an implementation that went back to rehashing in one pass would fail
    // it immediately.
    CountedMap map;
    int worst_case_moves = 0;
    for (int key = 0; key < 20000; ++key) {
        const int moves_before = CountedValue::moves;
        map.emplace(key, CountedValue(key));
        const int moves_for_this_operation = CountedValue::moves - moves_before;
        // One move is the value being copied into place; the rest are migration work.
        worst_case_moves = std::max(worst_case_moves, moves_for_this_operation);
        ASSERT_LE(static_cast<size_t>(moves_for_this_operation), CountedMap::migration_slots_per_operation + 1)
            << "operation " << key << " moved " << moves_for_this_operation << " entries, at a map size of " << map.size();
    }
    EXPECT_GT(worst_case_moves, 0) << "no migration happened, so the bound was never tested";
}

TEST_F(IncrementalRehashMapTest, TheBoundHoldsAcrossManyDoublings) {
    // 200,000 entries crosses a dozen doublings. The old behaviour was O(capacity) in the one
    // operation that crossed each of them, and that is what has to stay gone as the map grows.
    CountedMap map;
    for (int key = 0; key < 200000; ++key) {
        const int moves_before = CountedValue::moves;
        map.emplace(key, CountedValue(key));
        ASSERT_LE(static_cast<size_t>(CountedValue::moves - moves_before), CountedMap::migration_slots_per_operation + 1)
            << "the per-operation bound broke at a map size of " << map.size();
    }
    EXPECT_EQ(map.size(), 200000u);
}

TEST_F(IncrementalRehashMapTest, EachTableAllocationIsReportedOnceAndWhole) {
    AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1024;
    std::vector<size_t> reported_sizes;
    reporter.on_large_allocation = [&reported_sizes](size_t bytes, size_t largest) {
        reported_sizes.push_back(bytes);
        EXPECT_GE(largest, bytes);
    };

    IntMap map(0, std::hash<int>{}, std::equal_to<int>{}, &reporter);
    for (int key = 0; key < 20000; ++key) {
        map.emplace(key, key);
    }

    ASSERT_FALSE(reported_sizes.empty()) << "the reporter never fired, so growth is invisible again";
    EXPECT_EQ(map.size(), 20000u);
    // One report per table rather than one per array, and each table larger than the last, so
    // the log reads as a structure doubling rather than as unrelated allocations.
    for (size_t index = 1; index < reported_sizes.size(); ++index) {
        EXPECT_GT(reported_sizes[index], reported_sizes[index - 1]);
    }
    EXPECT_EQ(reporter.largest_allocation_bytes.load(), reported_sizes.back());
}

TEST_F(IncrementalRehashMapTest, AllocationsBelowTheThresholdAreNotReported) {
    AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1024UL * 1024UL * 1024UL;
    size_t reports = 0;
    reporter.on_large_allocation = [&reports](size_t, size_t) { ++reports; };

    IntMap map(0, std::hash<int>{}, std::equal_to<int>{}, &reporter);
    for (int key = 0; key < 1000; ++key) {
        map.emplace(key, key);
    }
    EXPECT_EQ(reports, 0u) << "a small table was reported, which is the noise the threshold exists to prevent";
}

TEST_F(IncrementalRehashMapTest, AMapWithNoReporterIsUnaffected) {
    IntMap map(0, std::hash<int>{}, std::equal_to<int>{}, nullptr);
    for (int key = 0; key < 5000; ++key) {
        map.emplace(key, key);
    }
    EXPECT_EQ(map.size(), 5000u);
}

TEST_F(IncrementalRehashMapTest, RandomisedOperationsAgreeWithUnorderedMap) {
    // Differential testing against the standard library. Three seeds and three shapes:
    // insert-heavy, erase-heavy, and balanced churn over a small key space so that keys are
    // reused and tombstones accumulate.
    const std::vector<uint32_t> seeds{1U, 20260811U, 987654321U};
    const std::vector<int> erase_weights{1, 5, 8};

    for (size_t shape = 0; shape < seeds.size(); ++shape) {
        StringMap map;
        std::unordered_map<int, std::string> oracle;
        std::mt19937 generator(seeds[shape]);
        const int erase_weight = erase_weights[shape];

        for (int round = 0; round < 50000; ++round) {
            const int key = static_cast<int>(generator() % 2000);
            const int action = static_cast<int>(generator() % 10);
            if (action < erase_weight) {
                EXPECT_EQ(map.erase(key), oracle.erase(key)) << "shape " << shape << " round " << round;
            } else if (action < erase_weight + 1) {
                const std::string value = "assigned" + std::to_string(round);
                map.insert_or_assign(key, value);
                oracle[key] = value;
            } else {
                const std::string value = "value" + std::to_string(key);
                map.emplace(key, value);
                oracle.emplace(key, value);
            }
            ASSERT_EQ(map.size(), oracle.size()) << "shape " << shape << " round " << round;
        }

        for (const auto& expected : oracle) {
            const auto found = map.find(expected.first);
            ASSERT_NE(found, map.end()) << "shape " << shape << ": entry " << expected.first << " missing";
            EXPECT_EQ(found->second, expected.second);
        }
        size_t visited = 0;
        for (const auto& entry : map) {
            const auto expected = oracle.find(entry.first);
            ASSERT_NE(expected, oracle.end()) << "shape " << shape << ": iteration produced an entry not in the oracle";
            EXPECT_EQ(entry.second, expected->second);
            ++visited;
        }
        EXPECT_EQ(visited, oracle.size()) << "shape " << shape;
    }
}

#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
TEST_F(IncrementalRehashMapTest, ASecondThreadIsRejected) {
    IntMap map;
    map.emplace(1, 1);

    bool threw_precondition = false;
    std::thread intruder([&map, &threw_precondition]() {
        try {
            (void)map.find(1);
        } catch (const PreconditionAssertion&) {
            threw_precondition = true;
        }
    });
    intruder.join();
    EXPECT_TRUE(threw_precondition) << "a second thread was allowed to touch a thread-confined map";
}

TEST_F(IncrementalRehashMapTest, ConfinementIsClaimedByWhicheverThreadTouchesItFirst) {
    IntMap map;
    bool inserted_from_other_thread = false;
    std::thread owner([&map, &inserted_from_other_thread]() {
        map.emplace(1, 1);
        inserted_from_other_thread = map.find(1) != map.end();
    });
    owner.join();
    EXPECT_TRUE(inserted_from_other_thread);
    // The map was claimed by the thread that has now finished, so this one is the intruder.
    EXPECT_THROW((void)map.find(1), PreconditionAssertion);
}
#endif

} // namespaces
