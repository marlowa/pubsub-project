// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "KeptReportStore.hpp"

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

using Clock = sequencer::KeptReportStore::Clock;
using std::chrono::milliseconds;

const Clock::time_point start{};

// The contents of a report numbered seq_no: size bytes, each holding the low byte of seq_no plus its
// position, so that a copy overwritten in part, or handed back from the wrong place, does not match.
std::vector<uint8_t> report_bytes(int64_t seq_no, size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8_t>(seq_no + static_cast<int64_t>(index));
    }
    return bytes;
}

// Keeps report seq_no, of the given size, as arriving seq_no milliseconds after start. Every copy is
// treated as still needed, so every overwrite is counted as lost.
void keep(sequencer::KeptReportStore& store, int64_t seq_no, size_t size) {
    const std::vector<uint8_t> bytes = report_bytes(seq_no, size);
    store.keep(seq_no, bytes.data(), bytes.size(), start + milliseconds{seq_no}, start);
}

// Takes every copy out of the store, checks each one's contents, and returns their sequence numbers in the order handed back.
std::vector<int64_t> take_all(sequencer::KeptReportStore& store) {
    std::vector<int64_t> taken;
    store.take_all([&taken](int64_t seq_no, const uint8_t* bytes, size_t size) {
        const std::vector<uint8_t> expected = report_bytes(seq_no, size);
        EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + size), expected) << "report " << seq_no << " came back with different contents";
        taken.push_back(seq_no);
    });
    return taken;
}

} // un-named namespace

TEST(KeptReportStoreTest, CopiesComeBackOldestFirstAndTheStoreIsThenEmpty) {
    sequencer::KeptReportStore store(1000, 100);
    keep(store, 1, 10);
    keep(store, 2, 20);
    keep(store, 3, 30);
    EXPECT_EQ(store.count(), 3U);
    EXPECT_EQ(store.bytes_used(), 60U);

    EXPECT_EQ(take_all(store), (std::vector<int64_t>{1, 2, 3}));
    EXPECT_EQ(store.count(), 0U);
    EXPECT_EQ(store.bytes_used(), 0U);
    EXPECT_EQ(store.lost(), 0);
}

TEST(KeptReportStoreTest, WhenTheBlockIsFullTheOldestCopiesAreOverwrittenAndCountedAsLost) {
    // Room for three copies of 30 bytes, but not four.
    sequencer::KeptReportStore store(100, 100);
    keep(store, 1, 30);
    keep(store, 2, 30);
    keep(store, 3, 30);
    keep(store, 4, 30);
    EXPECT_EQ(store.lost(), 1);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{2, 3, 4}));
}

TEST(KeptReportStoreTest, WhenTheListIsFullTheOldestCopyIsOverwrittenEvenIfTheBlockHasRoom) {
    sequencer::KeptReportStore store(1000, 2);
    keep(store, 1, 10);
    keep(store, 2, 10);
    keep(store, 3, 10);
    EXPECT_EQ(store.lost(), 1);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{2, 3}));
}

TEST(KeptReportStoreTest, ACopyIsNeverSplitAcrossTheEndOfTheBlock) {
    // Two copies of 40 leave 20 bytes at the end. A third of 40 does not fit there, so it is written
    // at the start, which needs the first copy overwritten -- and only the first.
    sequencer::KeptReportStore store(100, 100);
    keep(store, 1, 40);
    keep(store, 2, 40);
    keep(store, 3, 40);
    EXPECT_EQ(store.lost(), 1);
    EXPECT_EQ(store.bytes_used(), 80U);

    // A copy of 20 does not fit between the third copy, ending at 40, and the second, starting at 40,
    // so the second is overwritten too. The store is then back in one stretch from 0.
    keep(store, 4, 20);
    EXPECT_EQ(store.lost(), 2);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{3, 4}));
}

TEST(KeptReportStoreTest, OverwritingACopyNoLongerNeededIsNotALoss) {
    sequencer::KeptReportStore store(100, 100);
    const std::vector<uint8_t> bytes = report_bytes(1, 60);
    store.keep(1, bytes.data(), bytes.size(), start + milliseconds{1}, start);
    // The second arrives when only copies from 50 ms on are needed, so the first may be overwritten freely.
    const std::vector<uint8_t> second = report_bytes(2, 60);
    store.keep(2, second.data(), second.size(), start + milliseconds{60}, start + milliseconds{50});
    EXPECT_EQ(store.lost(), 0);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{2}));
}

TEST(KeptReportStoreTest, AReportLargerThanTheBlockIsNotKeptAndIsCountedAsLost) {
    sequencer::KeptReportStore store(100, 100);
    keep(store, 1, 10);
    keep(store, 2, 101);
    EXPECT_EQ(store.lost(), 1);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{1}));
}

TEST(KeptReportStoreTest, DiscardingByAgeRemovesOnlyCopiesThatArrivedBeforeTheCutoff) {
    sequencer::KeptReportStore store(1000, 100);
    for (int64_t seq_no = 1; seq_no <= 10; ++seq_no) {
        keep(store, seq_no, 10);
    }
    store.discard_received_before(start + milliseconds{4});
    EXPECT_EQ(store.lost(), 0);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{4, 5, 6, 7, 8, 9, 10}));
}

TEST(KeptReportStoreTest, ACapacityOfZeroBytesIsRefused) {
    EXPECT_THROW(sequencer::KeptReportStore(0, 10), pubsub_itc_fw::PreconditionAssertion);
}

// Many copies of random sizes, against a plain list of what the store should hold. Each copy that
// does not fit is made room for by removing the oldest from the plain list as well, with the room
// worked out independently from the offsets the plain list records.
TEST(KeptReportStoreTest, RandomSizesAgreeWithASimpleModel) {
    const size_t capacity_bytes = 997;
    const size_t capacity_reports = 40;
    sequencer::KeptReportStore store(capacity_bytes, capacity_reports);

    struct Modelled {
        int64_t seq_no;
        size_t offset;
        size_t size;
    };
    std::vector<Modelled> model;
    size_t model_next = 0;
    int64_t model_lost = 0;

    // Whether the bytes [offset, offset + size) overlap any copy the model holds.
    auto overlaps = [&model](size_t offset, size_t size) {
        for (const Modelled& held : model) {
            if (offset < held.offset + held.size && held.offset < offset + size) {
                return true;
            }
        }
        return false;
    };

    std::mt19937 random(12345);
    std::uniform_int_distribution<size_t> sizes(1, 120);
    for (int64_t seq_no = 1; seq_no <= 20000; ++seq_no) {
        const size_t size = sizes(random);

        // Where the model writes it: after the newest copy if it fits before the end of the block,
        // otherwise at the start; then overwrite the oldest copies until nothing overlaps and the
        // list has room.
        for (;;) {
            if (model.empty()) {
                model_next = 0;
            }
            size_t offset = (capacity_bytes - model_next >= size) ? model_next : 0;
            if (model.size() < capacity_reports && !overlaps(offset, size)) {
                model.push_back(Modelled{seq_no, offset, size});
                model_next = offset + size;
                break;
            }
            model.erase(model.begin());
            ++model_lost;
        }
        keep(store, seq_no, size);

        ASSERT_EQ(store.count(), model.size()) << "after report " << seq_no;
        ASSERT_EQ(store.lost(), model_lost) << "after report " << seq_no;

        // Now and then, take everything out and compare it with the model.
        if (seq_no % 997 == 0) {
            std::vector<int64_t> expected;
            for (const Modelled& held : model) {
                expected.push_back(held.seq_no);
            }
            ASSERT_EQ(take_all(store), expected) << "after report " << seq_no;
            model.clear();
        }
    }
}

TEST(KeptReportStoreTest, CopiesTheLeaderHasForwardedAreDiscardedUpToThePositionItGives) {
    sequencer::KeptReportStore store(1000, 100);
    for (int64_t number = 1; number <= 5; ++number) {
        const std::vector<uint8_t> bytes = report_bytes(number, 10);
        store.keep(number, bytes.data(), bytes.size(), start + milliseconds{number}, start, sequencer::EngineReportPosition{3, number});
    }
    EXPECT_EQ(store.discard_through(sequencer::EngineReportPosition{3, 3}), 3U);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{4, 5}));
}

TEST(KeptReportStoreTest, APositionInAnEarlierEngineLeadershipCoversNothingInALaterOne) {
    sequencer::KeptReportStore store(1000, 100);
    const std::vector<uint8_t> first = report_bytes(1, 10);
    const std::vector<uint8_t> second = report_bytes(2, 10);
    store.keep(1, first.data(), first.size(), start + milliseconds{1}, start, sequencer::EngineReportPosition{3, 900});
    // A new engine leadership numbers from 1 again, under a higher epoch.
    store.keep(2, second.data(), second.size(), start + milliseconds{2}, start, sequencer::EngineReportPosition{4, 1});
    EXPECT_EQ(store.discard_through(sequencer::EngineReportPosition{3, 1000}), 1U);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{2}));
}

TEST(KeptReportStoreTest, ACopyThatDoesNotSayWhereItStandsIsNotDiscardedByPosition) {
    sequencer::KeptReportStore store(1000, 100);
    keep(store, 1, 10);
    EXPECT_EQ(store.discard_through(sequencer::EngineReportPosition{9, 1000}), 0U);
    EXPECT_EQ(take_all(store), (std::vector<int64_t>{1}));
}
