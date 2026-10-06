// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t plenty_of_bytes = 1024 * 1024;

// A send whose chunk is not a real allocation: for tests that never return chunks to an allocator.
WaitingSend fake_send(uintptr_t marker, uint32_t byte_count) {
    return WaitingSend{nullptr, invalid_slab_handle, reinterpret_cast<void*>(marker), byte_count};
}

} // un-named namespace

TEST(WaitingSendQueueTest, AnEmptyQueueHoldsNoStorage) {
    const WaitingSendQueue queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0U);
    EXPECT_EQ(queue.bytes(), 0U);
    EXPECT_EQ(queue.capacity(), 0U);
}

TEST(WaitingSendQueueTest, SendsLeaveInTheOrderTheyWereAdded) {
    WaitingSendQueue queue;
    for (uintptr_t marker = 1; marker <= 1000; ++marker) {
        ASSERT_TRUE(queue.add(fake_send(marker, 10), 65536, plenty_of_bytes));
    }
    EXPECT_EQ(queue.size(), 1000U);
    EXPECT_EQ(queue.bytes(), 10000U);
    for (uintptr_t marker = 1; marker <= 1000; ++marker) {
        ASSERT_EQ(queue.front().chunk, reinterpret_cast<void*>(marker));
        queue.pop_front();
    }
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.bytes(), 0U);
}

TEST(WaitingSendQueueTest, StorageStartsSmallDoublesAsItFillsAndIsReleasedWhenEmpty) {
    WaitingSendQueue queue;
    ASSERT_TRUE(queue.add(fake_send(1, 1), 65536, plenty_of_bytes));
    EXPECT_EQ(queue.capacity(), 64U);
    for (uintptr_t marker = 2; marker <= 65; ++marker) {
        ASSERT_TRUE(queue.add(fake_send(marker, 1), 65536, plenty_of_bytes));
    }
    EXPECT_EQ(queue.capacity(), 128U);
    while (!queue.empty()) {
        queue.pop_front();
    }
    EXPECT_EQ(queue.capacity(), 0U) << "the storage was kept after the queue emptied";
}

TEST(WaitingSendQueueTest, GrowingStopsAtTheLimitOnTheNumberOfSends) {
    WaitingSendQueue queue;
    for (uintptr_t marker = 1; marker <= 100; ++marker) {
        ASSERT_TRUE(queue.add(fake_send(marker, 1), 100, plenty_of_bytes));
    }
    EXPECT_EQ(queue.capacity(), 100U) << "the storage grew past the limit";
    EXPECT_FALSE(queue.add(fake_send(101, 1), 100, plenty_of_bytes)) << "a send past the limit on the number of sends was accepted";
    EXPECT_EQ(queue.size(), 100U);
    EXPECT_EQ(queue.front().chunk, reinterpret_cast<void*>(1)) << "a refused send changed the queue";
}

TEST(WaitingSendQueueTest, ASendThatWouldPassTheByteLimitIsRefused) {
    WaitingSendQueue queue;
    ASSERT_TRUE(queue.add(fake_send(1, 600), 65536, 1000));
    EXPECT_FALSE(queue.add(fake_send(2, 401), 65536, 1000));
    EXPECT_TRUE(queue.add(fake_send(3, 400), 65536, 1000)) << "a send that reaches the byte limit exactly was refused";
    EXPECT_EQ(queue.bytes(), 1000U);
}

TEST(WaitingSendQueueTest, AnEmptyQueueAcceptsOneSendLargerThanTheByteLimit) {
    WaitingSendQueue queue;
    EXPECT_TRUE(queue.add(fake_send(1, 5000), 65536, 1000)) << "a single large send would be refused for ever";
    EXPECT_FALSE(queue.add(fake_send(2, 1), 65536, 1000));
}

TEST(WaitingSendQueueTest, ALimitOfZeroSendsIsRefused) {
    WaitingSendQueue queue;
    EXPECT_THROW(static_cast<void>(queue.add(fake_send(1, 1), 0, plenty_of_bytes)), PreconditionAssertion);
}

TEST(WaitingSendQueueTest, FrontAndPopFrontOfAnEmptyQueueAreRefused) {
    WaitingSendQueue queue;
    EXPECT_THROW(static_cast<void>(queue.front()), PreconditionAssertion);
    EXPECT_THROW(queue.pop_front(), PreconditionAssertion);
}

TEST(WaitingSendQueueTest, ReleaseAllReturnsEveryChunkToItsAllocator) {
    // Small slabs, so that the chunks held span several of them.
    ExpandableSlabAllocator allocator(4096);
    WaitingSendQueue queue;
    constexpr uint32_t chunk_bytes = 512;
    for (int index = 0; index < 64; ++index) {
        auto [slab_id, chunk] = allocator.allocate(chunk_bytes);
        ASSERT_TRUE(queue.add(WaitingSend{&allocator, slab_id, chunk, chunk_bytes}, 65536, plenty_of_bytes));
    }
    const int slabs_holding_the_chunks = allocator.slab_count();
    ASSERT_GT(slabs_holding_the_chunks, 1);

    queue.release_all();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.bytes(), 0U);

    // The same chunks again must fit, nearly, in the slabs already there. The allocator holds back the
    // slab it emptied most recently until its next reclamation (see ExpandableSlabAllocator.hpp), so
    // one slab more than before is expected. Had release_all returned nothing, the allocator would need
    // as many slabs again as the chunks first took.
    std::vector<std::tuple<SlabHandle, void*>> again;
    for (int index = 0; index < 64; ++index) {
        again.push_back(allocator.allocate(chunk_bytes));
    }
    EXPECT_LE(allocator.slab_count(), slabs_holding_the_chunks + 1) << "the chunks were not returned to the allocator";
    for (const auto& [slab_id, chunk] : again) {
        allocator.deallocate(slab_id, chunk);
    }
}

} // namespaces
