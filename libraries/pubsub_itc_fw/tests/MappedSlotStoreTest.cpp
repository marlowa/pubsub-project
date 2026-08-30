// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file MappedSlotStoreTest.cpp
 * @brief Unit tests for pubsub_itc_fw::MappedSlotStore.
 *
 * The store exists so that a component's state outlives the process holding it, so most
 * of these tests close the store and open it again -- which is what a restart does, minus
 * the process death. The tests after the straightforward ones damage the file first:
 * a store whose free list is nonsense, whose slot states are nonsense, or which was
 * written by something else entirely, because those are the states a crash leaves behind
 * and the recovery path is the reason the class exists.
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MappedSlotStore.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw::tests {

namespace {

using SlotIndex = MappedSlotStore::SlotIndex;

constexpr uint32_t record_size = 24;
constexpr SlotIndex slot_count = 8;

// The file layout, mirrored here so that the tests below can damage a store the way a
// crash does. LayoutStillAgreesWithTheStore fails if the class moves anything.
constexpr size_t header_size = 32;
constexpr size_t offset_magic = 0;
constexpr size_t offset_version = 4;
constexpr size_t offset_payload_size = 8;
constexpr size_t offset_slot_count = 12;
constexpr size_t offset_published = 16;
constexpr size_t offset_free_head = 24;
constexpr size_t slot_header_size = 16;
constexpr size_t offset_slot_state = 0;
constexpr size_t offset_slot_next_free = 4;
constexpr size_t offset_slot_seq_no = 8;

constexpr size_t stride_for(uint32_t payload_size) {
    return (slot_header_size + payload_size + 7u) & ~static_cast<size_t>(7u);
}

constexpr size_t slot_offset(SlotIndex index, uint32_t payload_size = record_size) {
    return header_size + index * stride_for(payload_size);
}

// Fill a record with something that differs from slot to slot, so that a store read back
// with the wrong stride produces a mismatch rather than the same bytes in every slot.
std::vector<uint8_t> pattern(uint8_t seed, uint32_t size = record_size) {
    std::vector<uint8_t> bytes(size);
    for (uint32_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>(seed * 31u + i);
    }
    return bytes;
}

} // namespaces

class MappedSlotStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::string tmpl = "/dev/shm/mapped_slot_store_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir_ = tmpl;
        path_ = dir_ + "/store.bin";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // Put one record in the store and settle it, the way a caller is meant to.
    SlotIndex store_one(MappedSlotStore& store, uint8_t seed, int64_t seq_no) {
        const SlotIndex index = store.acquire();
        EXPECT_NE(index, MappedSlotStore::no_slot);
        const std::vector<uint8_t> bytes = pattern(seed, store.payload_size());
        std::memcpy(store.payload(index), bytes.data(), bytes.size());
        store.commit(index, seq_no);
        store.publish(seq_no);
        return index;
    }

    void expect_payload(const MappedSlotStore& store, SlotIndex index, uint8_t seed) {
        const std::vector<uint8_t> expected = pattern(seed, store.payload_size());
        EXPECT_EQ(std::memcmp(store.payload(index), expected.data(), expected.size()), 0) << "slot " << index;
    }

    template <typename T> T read_at(size_t offset) {
        std::ifstream in(path_, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        T value{};
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        EXPECT_TRUE(in.good()) << "could not read at " << offset;
        return value;
    }

    template <typename T> void write_at(size_t offset, T value) {
        std::fstream out(path_, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(static_cast<std::streamoff>(offset));
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        ASSERT_TRUE(out.good()) << "could not write at " << offset;
    }

    std::string dir_;
    std::string path_;
};

// ---- the store as it is meant to be used --------------------------------------------

TEST_F(MappedSlotStoreTest, OpenCreatesAStoreAndSaysSo) {
    MappedSlotStore store;
    EXPECT_FALSE(store.is_open());
    EXPECT_FALSE(store.open(path_, record_size, slot_count));
    EXPECT_TRUE(store.is_open());
    EXPECT_EQ(store.capacity(), slot_count);
    EXPECT_EQ(store.payload_size(), record_size);
    EXPECT_EQ(store.published(), 0);
    EXPECT_TRUE(std::filesystem::exists(path_));
}

TEST_F(MappedSlotStoreTest, OpeningAgainSaysTheStoreWasAlreadyThere) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
    }
    MappedSlotStore store;
    EXPECT_TRUE(store.open(path_, record_size, slot_count));
}

TEST_F(MappedSlotStoreTest, CloseLeavesTheStoreShutAndReopenable) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));
    store.close();
    EXPECT_FALSE(store.is_open());
    store.close(); // closing twice is not an error
    EXPECT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_TRUE(store.is_open());
}

TEST_F(MappedSlotStoreTest, ARecordSurvivesTheStoreBeingClosedAndOpenedAgain) {
    SlotIndex written = MappedSlotStore::no_slot;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        written = store_one(store, 7, 100);
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.published(), 100);
    EXPECT_EQ(store.rebuild_free_list(), 1u);
    EXPECT_TRUE(store.is_live(written));
    EXPECT_TRUE(store.is_recoverable(written));
    EXPECT_EQ(store.slot_seq_no(written), 100);
    expect_payload(store, written, 7);
}

TEST_F(MappedSlotStoreTest, EveryRecordComesBackWithItsOwnBytes) {
    std::vector<SlotIndex> written;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        for (uint8_t i = 0; i < slot_count; ++i) {
            written.push_back(store_one(store, i, 10 + i));
        }
        EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    ASSERT_EQ(store.rebuild_free_list(), slot_count);
    for (uint8_t i = 0; i < slot_count; ++i) {
        expect_payload(store, written[i], i);
        EXPECT_EQ(store.slot_seq_no(written[i]), 10 + i);
    }
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, AcquireHandsOutEverySlotExactlyOnce) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));

    std::set<SlotIndex> seen;
    for (SlotIndex i = 0; i < slot_count; ++i) {
        const SlotIndex index = store.acquire();
        ASSERT_NE(index, MappedSlotStore::no_slot);
        ASSERT_LT(index, slot_count);
        EXPECT_TRUE(seen.insert(index).second) << "slot " << index << " handed out twice";
    }
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, AReleasedSlotIsHandedOutAgain) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));

    std::vector<SlotIndex> held;
    for (SlotIndex i = 0; i < slot_count; ++i) {
        held.push_back(store.acquire());
    }
    ASSERT_EQ(store.acquire(), MappedSlotStore::no_slot);

    store.release(held[3]);
    EXPECT_FALSE(store.is_live(held[3]));
    const SlotIndex again = store.acquire();
    EXPECT_EQ(again, held[3]);
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, AReleasedRecordIsNotRecoveredAfterReopening) {
    SlotIndex kept = MappedSlotStore::no_slot;
    SlotIndex dropped = MappedSlotStore::no_slot;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        kept = store_one(store, 1, 50);
        dropped = store_one(store, 2, 51);
        store.release(dropped);
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.rebuild_free_list(), 1u);
    EXPECT_TRUE(store.is_recoverable(kept));
    EXPECT_FALSE(store.is_recoverable(dropped));
}

TEST_F(MappedSlotStoreTest, WarmLeavesTheContentAlone) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));
    const SlotIndex index = store_one(store, 5, 9);
    store.warm();
    expect_payload(store, index, 5);
    EXPECT_EQ(store.published(), 9);
    EXPECT_TRUE(store.is_recoverable(index));
}

TEST_F(MappedSlotStoreTest, TheConstPayloadIsTheSameBytes) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));
    const SlotIndex index = store_one(store, 3, 4);
    const MappedSlotStore& const_store = store;
    EXPECT_EQ(const_store.payload(index), store.payload(index));
    expect_payload(const_store, index, 3);
}

// ---- what a crash leaves behind ------------------------------------------------------

TEST_F(MappedSlotStoreTest, ARecordAboveThePublishedPositionIsNotRecovered) {
    // The caller died between committing the record and publishing it, so nobody was ever
    // told about the record and whatever produced it will produce it again.
    SlotIndex settled = MappedSlotStore::no_slot;
    SlotIndex unfinished = MappedSlotStore::no_slot;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        settled = store_one(store, 1, 200);
        unfinished = store.acquire();
        std::memcpy(store.payload(unfinished), pattern(2).data(), record_size);
        store.commit(unfinished, 201);
        // no publish
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.published(), 200);
    EXPECT_TRUE(store.is_live(unfinished));
    EXPECT_FALSE(store.is_recoverable(unfinished));
    EXPECT_TRUE(store.is_recoverable(settled));

    // and the slot it used goes back into circulation
    EXPECT_EQ(store.rebuild_free_list(), 1u);
    EXPECT_FALSE(store.is_live(unfinished));
}

TEST_F(MappedSlotStoreTest, ARecordStampedZeroIsNotRecovered) {
    // A new store publishes zero, so a slot stamped zero would otherwise be trusted by
    // every reader of a store nothing has yet been published to.
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        const SlotIndex index = store.acquire();
        store.commit(index, 0);
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.published(), 0);
    EXPECT_TRUE(store.is_live(0));
    EXPECT_FALSE(store.is_recoverable(0));
    EXPECT_EQ(store.rebuild_free_list(), 0u);
}

TEST_F(MappedSlotStoreTest, ASlotTakenButNeverCommittedIsReclaimed) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        for (SlotIndex i = 0; i < slot_count; ++i) {
            const SlotIndex taken = store.acquire(); // taken, never made live
            ASSERT_NE(taken, MappedSlotStore::no_slot);
        }
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.rebuild_free_list(), 0u);

    std::set<SlotIndex> seen;
    for (SlotIndex i = 0; i < slot_count; ++i) {
        const SlotIndex index = store.acquire();
        ASSERT_NE(index, MappedSlotStore::no_slot);
        EXPECT_TRUE(seen.insert(index).second);
    }
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, RebuildingHandsSlotsBackInAscendingOrder) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, slot_count));
    store_one(store, 1, 10); // slot 0
    store_one(store, 2, 11); // slot 1
    ASSERT_EQ(store.rebuild_free_list(), 2u);
    EXPECT_EQ(store.acquire(), 2u);
    EXPECT_EQ(store.acquire(), 3u);
}

TEST_F(MappedSlotStoreTest, ANonsenseFreeListIsRepairedByRebuilding) {
    // The free list is written on every acquire and release, so a process dying in the
    // middle of one can leave it holding anything at all.
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
    }
    write_at<uint32_t>(offset_free_head, 0xDEADBEEFu);
    write_at<uint32_t>(slot_offset(4) + offset_slot_next_free, 4u); // a slot pointing at itself

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.rebuild_free_list(), 1u);

    std::set<SlotIndex> seen;
    for (SlotIndex i = 0; i + 1 < slot_count; ++i) {
        const SlotIndex index = store.acquire();
        ASSERT_NE(index, MappedSlotStore::no_slot) << "ran out after " << i;
        ASSERT_LT(index, slot_count);
        EXPECT_TRUE(seen.insert(index).second) << "slot " << index << " handed out twice";
    }
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, ASlotWithAnUnrecognisedStateIsNotRecovered) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
        store_one(store, 2, 11);
    }
    write_at<uint32_t>(slot_offset(1) + offset_slot_state, 0x5A5A5A5Au);

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_FALSE(store.is_live(1));
    EXPECT_EQ(store.rebuild_free_list(), 1u);
}

TEST_F(MappedSlotStoreTest, ARecordStampedInTheFutureIsNotRecovered) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
    }
    write_at<int64_t>(slot_offset(0) + offset_slot_seq_no, 999999);

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_TRUE(store.is_live(0));
    EXPECT_FALSE(store.is_recoverable(0));
    EXPECT_EQ(store.rebuild_free_list(), 0u);
}

TEST_F(MappedSlotStoreTest, ANegativeStampIsNotRecovered) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
    }
    write_at<int64_t>(slot_offset(0) + offset_slot_seq_no, -1);

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, slot_count));
    EXPECT_FALSE(store.is_recoverable(0));
}

// ---- stores that must be refused -----------------------------------------------------

TEST_F(MappedSlotStoreTest, AStoreWrittenWithADifferentRecordSizeIsRefused) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
    }
    MappedSlotStore store;
    EXPECT_THROW(store.open(path_, record_size + 8, slot_count), PubSubItcException);
    EXPECT_FALSE(store.is_open());
}

TEST_F(MappedSlotStoreTest, AStoreWrittenWithADifferentSlotCountIsRefused) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
    }
    MappedSlotStore store;
    EXPECT_THROW(store.open(path_, record_size, slot_count * 2), PubSubItcException);
}

TEST_F(MappedSlotStoreTest, AStoreWrittenByAnEarlierVersionIsRefused) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
    }
    write_at<uint32_t>(offset_version, 99u);

    MappedSlotStore store;
    EXPECT_THROW(store.open(path_, record_size, slot_count), PubSubItcException);
}

TEST_F(MappedSlotStoreTest, TheReasonForRefusingSaysWhatWasHeldAndWhatWasAsked) {
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
    }
    MappedSlotStore store;
    try {
        store.open(path_, record_size, slot_count * 2);
        FAIL() << "expected the mismatched store to be refused";
    } catch (const PubSubItcException& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find(path_), std::string::npos) << what;
        EXPECT_NE(what.find(std::to_string(slot_count)), std::string::npos) << what;
        EXPECT_NE(what.find(std::to_string(slot_count * 2)), std::string::npos) << what;
    }
}

TEST_F(MappedSlotStoreTest, AFileThatWasNeverAStoreIsTakenOver) {
    {
        std::ofstream out(path_, std::ios::binary);
        const std::string junk(200, 'x');
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    MappedSlotStore store;
    EXPECT_FALSE(store.open(path_, record_size, slot_count));
    EXPECT_EQ(store.published(), 0);
    EXPECT_EQ(store.rebuild_free_list(), 0u);
    EXPECT_NE(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, AFileTooShortToHoldAHeaderIsTakenOver) {
    {
        std::ofstream out(path_, std::ios::binary);
        out.put('x');
    }
    MappedSlotStore store;
    EXPECT_FALSE(store.open(path_, record_size, slot_count));
    EXPECT_NE(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, AStoreWithNoRecordSizeOrNoSlotsIsRefused) {
    // Asking for these is a mistake in the caller rather than something wrong with the
    // file, so it is a precondition and not the exception the other refusals use.
    MappedSlotStore store;
    EXPECT_THROW(store.open(path_, 0, slot_count), PreconditionAssertion);
    EXPECT_THROW(store.open(path_, record_size, 0), PreconditionAssertion);
    EXPECT_THROW(store.open(path_, record_size, MappedSlotStore::no_slot), PreconditionAssertion);
    EXPECT_FALSE(store.is_open());
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(MappedSlotStoreTest, APathThatCannotBeOpenedIsReported) {
    MappedSlotStore store;
    EXPECT_THROW(store.open(dir_ + "/no/such/directory/store.bin", record_size, slot_count), PubSubItcException);
    EXPECT_FALSE(store.is_open());
}

TEST_F(MappedSlotStoreTest, AFileOfNoLengthIsTakenOver) {
    // A file left behind at zero length holds nothing, so it is a new store and not one
    // whose header has to agree with what is being asked for.
    { std::ofstream out(path_, std::ios::binary); }
    MappedSlotStore store;
    EXPECT_FALSE(store.open(path_, record_size, slot_count));
    EXPECT_NE(store.acquire(), MappedSlotStore::no_slot);
}

TEST_F(MappedSlotStoreTest, APathThatCannotBeGivenALengthIsReported) {
    // A named pipe can be opened but cannot be given a length, which is the failure
    // between opening the file and mapping it.
    const std::string pipe_path = dir_ + "/pipe";
    ASSERT_EQ(::mkfifo(pipe_path.c_str(), 0644), 0);
    MappedSlotStore store;
    EXPECT_THROW(store.open(pipe_path, record_size, slot_count), PubSubItcException);
    EXPECT_FALSE(store.is_open());
}

TEST_F(MappedSlotStoreTest, AStoreTooLargeToMapIsReported) {
    // Half a gigabyte a record and a thousand million of them: the file can be given
    // that length but no address space can hold it.
    constexpr uint32_t huge_record = 1u << 29;
    constexpr SlotIndex huge_count = 1u << 30;
    MappedSlotStore store;
    EXPECT_THROW(store.open(path_, huge_record, huge_count), PubSubItcException);
    EXPECT_FALSE(store.is_open());
}

// ---- shapes and sizes ----------------------------------------------------------------

TEST_F(MappedSlotStoreTest, ARecordSizeThatIsNotAMultipleOfEightStillRoundTrips) {
    constexpr uint32_t odd_size = 13;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, odd_size, slot_count));
        for (uint8_t i = 0; i < slot_count; ++i) {
            store_one(store, i, 100 + i);
        }
    }
    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, odd_size, slot_count));
    ASSERT_EQ(store.rebuild_free_list(), slot_count);
    for (uint8_t i = 0; i < slot_count; ++i) {
        expect_payload(store, i, i);
    }
}

TEST_F(MappedSlotStoreTest, AStoreOfOneSlotWorks) {
    MappedSlotStore store;
    ASSERT_FALSE(store.open(path_, record_size, 1));
    const SlotIndex index = store_one(store, 1, 5);
    EXPECT_EQ(index, 0u);
    EXPECT_EQ(store.acquire(), MappedSlotStore::no_slot);
    store.release(index);
    EXPECT_EQ(store.acquire(), 0u);
}

TEST_F(MappedSlotStoreTest, ARecordLargerThanAPageRoundTrips) {
    constexpr uint32_t big = 9000;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, big, 4));
        store_one(store, 1, 10);
        store_one(store, 2, 11);
    }
    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, big, 4));
    ASSERT_EQ(store.rebuild_free_list(), 2u);
    expect_payload(store, 0, 1);
    expect_payload(store, 1, 2);
}

TEST_F(MappedSlotStoreTest, ManyRecordsSurviveWithHolesInThem) {
    constexpr SlotIndex many = 5000;
    std::set<SlotIndex> expected_live;
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, many));
        for (SlotIndex i = 0; i < many; ++i) {
            const SlotIndex index = store.acquire();
            ASSERT_NE(index, MappedSlotStore::no_slot);
            std::memcpy(store.payload(index), pattern(static_cast<uint8_t>(i)).data(), record_size);
            store.commit(index, static_cast<int64_t>(i) + 1);
            expected_live.insert(index);
        }
        store.publish(static_cast<int64_t>(many));
        for (SlotIndex i = 0; i < many; i += 3) {
            store.release(i);
            expected_live.erase(i);
        }
    }

    MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, record_size, many));
    EXPECT_EQ(store.rebuild_free_list(), expected_live.size());
    for (SlotIndex i = 0; i < many; ++i) {
        EXPECT_EQ(store.is_recoverable(i), expected_live.count(i) == 1) << "slot " << i;
        if (expected_live.count(i) == 1) {
            expect_payload(store, i, static_cast<uint8_t>(i));
        }
    }
}

TEST_F(MappedSlotStoreTest, LayoutStillAgreesWithTheStore) {
    // The damage tests above write to offsets worked out here rather than asked of the
    // class, which hides its layout. If this fails they are damaging the wrong bytes and
    // proving nothing, so it fails loudly rather than letting them pass for free.
    {
        MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, record_size, slot_count));
        store_one(store, 1, 10);
        store_one(store, 2, 4242);
    }

    EXPECT_EQ(read_at<uint32_t>(offset_payload_size), record_size);
    EXPECT_EQ(read_at<uint32_t>(offset_slot_count), slot_count);
    EXPECT_EQ(read_at<int64_t>(offset_published), 4242);
    EXPECT_EQ(read_at<uint32_t>(offset_magic), 0x534C4F54u);
    EXPECT_EQ(read_at<uint32_t>(offset_version), 1u);
    EXPECT_EQ(read_at<uint32_t>(slot_offset(1) + offset_slot_state), 1u);
    EXPECT_EQ(read_at<int64_t>(slot_offset(1) + offset_slot_seq_no), 4242);

    const std::vector<uint8_t> expected = pattern(2);
    std::array<uint8_t, record_size> held{};
    std::ifstream in(path_, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(slot_offset(1) + slot_header_size));
    in.read(reinterpret_cast<char*>(held.data()), record_size);
    EXPECT_EQ(std::memcmp(held.data(), expected.data(), record_size), 0);
}

} // namespaces
