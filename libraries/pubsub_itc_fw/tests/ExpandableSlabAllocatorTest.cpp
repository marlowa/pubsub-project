// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ExpandableSlabAllocatorTest.cpp
 * @brief Unit tests for ExpandableSlabAllocator.
 *
 * Tests cover:
 *   - Basic allocation and deallocation
 *   - Slab chaining when the current slab is exhausted
 *   - Slab reclamation (reset of current slab, destruction of old slabs)
 *   - Concurrent deallocation from multiple threads
 *   - Precondition violations (zero size, size exceeds slab, null ptr, bad slab_id)
 *   - Postcondition: allocate() never returns nullptr
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw::tests {

// ============================================================
// Fixture
// ============================================================

class ExpandableSlabAllocatorTest : public ::testing::Test {
  protected:
    // A convenient small slab size for tests that want to force chaining.
    static constexpr size_t kSmallSlab = 256;

    // A larger slab for tests that just want to allocate without chaining.
    static constexpr size_t kLargeSlab = 65536;
};

// ============================================================
// Construction
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, ConstructionZeroSizeThrows) {
    EXPECT_THROW(ExpandableSlabAllocator{0}, PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, ConstructionCreatesOneSlab) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    EXPECT_EQ(alloc.slab_count(), 1);
    EXPECT_EQ(alloc.slab_size(), kLargeSlab);
}

// ============================================================
// Basic allocation
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, AllocateReturnsNonNullPointer) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    auto [slab_id, ptr] = alloc.allocate(64);
    EXPECT_NE(ptr, nullptr);
    EXPECT_GE(slab_id, 0);
    alloc.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, ExpandableSlabAllocateZeroSizeThrows) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    EXPECT_THROW(alloc.allocate(0), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, AllocateExceedingSlabSizeThrows) {
    ExpandableSlabAllocator alloc{kSmallSlab};
    EXPECT_THROW(alloc.allocate(kSmallSlab + 1), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, AllocateExactSlabSizeSucceeds) {
    ExpandableSlabAllocator alloc{kSmallSlab};
    auto [slab_id, ptr] = alloc.allocate(kSmallSlab);
    EXPECT_NE(ptr, nullptr);
    alloc.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, MultipleAllocationsReturnDistinctPointers) {
    ExpandableSlabAllocator alloc{kLargeSlab};

    std::vector<std::pair<int, void*>> allocations;
    for (int i = 0; i < 16; ++i) {
        auto [slab_id, ptr] = alloc.allocate(64);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    // All pointers must be distinct.
    for (size_t i = 0; i < allocations.size(); ++i) {
        for (size_t j = i + 1; j < allocations.size(); ++j) {
            EXPECT_NE(allocations[i].second, allocations[j].second)
                << "duplicate pointer at indices " << i << " and " << j;
        }
    }

    for (auto [slab_id, ptr] : allocations) {
        alloc.deallocate(slab_id, ptr);
    }
}

TEST_F(ExpandableSlabAllocatorTest, AllocatedMemoryIsWritable) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    auto [slab_id, ptr] = alloc.allocate(128);
    ASSERT_NE(ptr, nullptr);

    // Write a pattern and read it back.
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < 128; ++i) {
        bytes[i] = static_cast<uint8_t>(i & 0xFF);
    }
    for (size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(bytes[i], static_cast<uint8_t>(i & 0xFF));
    }

    alloc.deallocate(slab_id, ptr);
}

// ============================================================
// Slab chaining
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, SlabChainsWhenCurrentIsFull) {
    // Each allocation is kSmallSlab bytes, so the first allocation fills
    // the slab entirely and the second must chain a new one.
    ExpandableSlabAllocator alloc{kSmallSlab};

    EXPECT_EQ(alloc.slab_count(), 1);

    auto [slab_id_0, ptr_0] = alloc.allocate(kSmallSlab);
    EXPECT_NE(ptr_0, nullptr);
    EXPECT_EQ(alloc.slab_count(), 1);

    // This allocation cannot fit in slab 0 — a new slab must be chained.
    auto [slab_id_1, ptr_1] = alloc.allocate(kSmallSlab);
    EXPECT_NE(ptr_1, nullptr);
    EXPECT_EQ(alloc.slab_count(), 2);
    EXPECT_NE(slab_id_0, slab_id_1);

    alloc.deallocate(slab_id_0, ptr_0);
    alloc.deallocate(slab_id_1, ptr_1);
}

TEST_F(ExpandableSlabAllocatorTest, SlabCountMonotonicallyIncreasesUnderLoad) {
    ExpandableSlabAllocator alloc{kSmallSlab};

    // Force many slab chains by filling each slab completely.
    // Keep all allocations live so no reclamation occurs.
    std::vector<std::pair<int, void*>> allocations;
    for (int i = 0; i < 8; ++i) {
        auto [slab_id, ptr] = alloc.allocate(kSmallSlab);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    EXPECT_EQ(alloc.slab_count(), 8);

    for (auto [slab_id, ptr] : allocations) {
        alloc.deallocate(slab_id, ptr);
    }
}

// ============================================================
// Slab reclamation
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, CurrentSlabIsResetWhenAllChunksFreed) {
    // Fill slab 0 completely, then free all chunks.
    // On the next allocate(), drain_empty_slab_queue() should reset slab 0
    // (it is still the current slab at that point since no chaining occurred).
    ExpandableSlabAllocator alloc{kSmallSlab};

    std::vector<std::pair<int, void*>> allocations;

    // Fill with small chunks until the slab is exhausted and a new one chains.
    // We use a chunk size that divides evenly into kSmallSlab.
    constexpr size_t kChunk = 64;
    const int chunks_per_slab = static_cast<int>(kSmallSlab / kChunk);

    for (int i = 0; i < chunks_per_slab; ++i) {
        auto [slab_id, ptr] = alloc.allocate(kChunk);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    // All chunks are from slab 0.
    for (auto [slab_id, ptr] : allocations) {
        EXPECT_EQ(slab_id, 0);
    }

    // Free all chunks — slab 0 will be enqueued in the empty slab queue.
    for (auto [slab_id, ptr] : allocations) {
        alloc.deallocate(slab_id, ptr);
    }

    // The next allocate() triggers drain_empty_slab_queue(), which resets slab 0.
    // Since slab 0 is still the current slab, it should be reused (not destroyed).
    auto [slab_id_new, ptr_new] = alloc.allocate(kChunk);
    EXPECT_NE(ptr_new, nullptr);
    EXPECT_EQ(slab_id_new, 0); // reused, not a new slab
    EXPECT_EQ(alloc.slab_count(), 1); // no extra slab created

    alloc.deallocate(slab_id_new, ptr_new);
}

TEST_F(ExpandableSlabAllocatorTest, OldSlabIsDestroyedAfterChaining) {
    // Fill slab 0, chain slab 1 with a partial allocation (leaving room for the
    // probe allocation below), then free the slab-0 chunk.
    // On the next allocate(), slab 0 (not current) should be destroyed,
    // leaving a nullptr entry in the registry.
    ExpandableSlabAllocator alloc{kSmallSlab};

    // Fill slab 0 completely.
    auto [slab_id_0, ptr_0] = alloc.allocate(kSmallSlab);
    ASSERT_NE(ptr_0, nullptr);
    EXPECT_EQ(slab_id_0, 0);

    // Chain slab 1 with a partial allocation — leave room for the probe below.
    constexpr size_t kPartial = 64;
    auto [slab_id_1, ptr_1] = alloc.allocate(kPartial);
    ASSERT_NE(ptr_1, nullptr);
    EXPECT_EQ(slab_id_1, 1);
    EXPECT_EQ(alloc.slab_count(), 2);

    // Free slab-0 chunk — enqueues slab 0 into the empty slab queue.
    alloc.deallocate(slab_id_0, ptr_0);

    // Next allocate() drains the queue and destroys slab 0 (not current).
    // Fits in slab 1 since we left room above — no third slab is chained.
    auto [slab_id_2, ptr_2] = alloc.allocate(kPartial);
    ASSERT_NE(ptr_2, nullptr);
    EXPECT_EQ(slab_id_2, 1);

    // Slab count still 2 (destroyed slab leaves a nullptr slot, not removed).
    EXPECT_EQ(alloc.slab_count(), 2);

    // Attempting to deallocate with the destroyed slab_id should throw.
    EXPECT_THROW(alloc.deallocate(slab_id_0, ptr_0), PreconditionAssertion);

    alloc.deallocate(slab_id_1, ptr_1);
    alloc.deallocate(slab_id_2, ptr_2);
}

// ============================================================
// Deallocation precondition violations
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, DeallocateNullPtrThrows) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    EXPECT_THROW(alloc.deallocate(0, nullptr), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateNegativeSlabIdThrows) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    auto [slab_id, ptr] = alloc.allocate(64);
    EXPECT_THROW(alloc.deallocate(-1, ptr), PreconditionAssertion);
    alloc.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateOutOfRangeSlabIdThrows) {
    ExpandableSlabAllocator alloc{kLargeSlab};
    auto [slab_id, ptr] = alloc.allocate(64);
    EXPECT_THROW(alloc.deallocate(999, ptr), PreconditionAssertion);
    alloc.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateDestroyedSlabThrows) {
    // Destroy slab 0 by chaining, then freeing all slab-0 chunks,
    // then triggering reclamation via a further allocate().
    ExpandableSlabAllocator alloc{kSmallSlab};

    // Fill slab 0 completely.
    auto [slab_id_0, ptr_0] = alloc.allocate(kSmallSlab);

    // Chain slab 1 with a partial allocation — leave room for the probe below.
    auto [slab_id_1, ptr_1] = alloc.allocate(64);

    // Free slab-0 chunk to enqueue it for destruction.
    alloc.deallocate(slab_id_0, ptr_0);

    // Trigger drain — slab 0 gets destroyed. Fits in slab 1, no third slab chained.
    auto [slab_id_2, ptr_2] = alloc.allocate(64);

    // Now attempting to deallocate from destroyed slab 0 must throw.
    EXPECT_THROW(alloc.deallocate(slab_id_0, ptr_0), PreconditionAssertion);

    alloc.deallocate(slab_id_1, ptr_1);
    alloc.deallocate(slab_id_2, ptr_2);
}

// ============================================================
// Concurrent deallocation
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, ConcurrentDeallocationsAreSafe) {
    // Allocate a large number of chunks from the reactor thread (this thread),
    // then deallocate them concurrently from multiple threads. Verify that
    // the allocator remains structurally sound afterwards.
    ExpandableSlabAllocator alloc{kLargeSlab};

    constexpr int kChunkSize = 64;
    constexpr int kNumChunks = 128;

    std::vector<std::pair<int, void*>> allocations;
    allocations.reserve(kNumChunks);

    for (int i = 0; i < kNumChunks; ++i) {
        auto [slab_id, ptr] = alloc.allocate(kChunkSize);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    // Divide chunks across threads and deallocate concurrently.
    constexpr int kNumThreads = 4;
    const int chunks_per_thread = kNumChunks / kNumThreads;

    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        int start = t * chunks_per_thread;
        int end = start + chunks_per_thread;
        threads.emplace_back([&alloc, &allocations, start, end]() {
            for (int i = start; i < end; ++i) {
                alloc.deallocate(allocations[i].first, allocations[i].second);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // After all deallocations, trigger reclamation and verify we can still allocate.
    auto [slab_id_after, ptr_after] = alloc.allocate(kChunkSize);
    EXPECT_NE(ptr_after, nullptr);
    alloc.deallocate(slab_id_after, ptr_after);
}

TEST_F(ExpandableSlabAllocatorTest, ConcurrentDeallocationsFromManySlabs) {
    // Force multiple slab chains by filling each slab completely, keeping all
    // allocations live so no reclamation occurs during setup.
    // Then deallocate all chunks concurrently from separate threads.
    // Finally, drain one chunk at a time from the reactor thread to verify
    // the allocator is structurally intact — reclamation is triggered
    // sequentially so the Vyukov MPSC queue is never in a multi-producer
    // mid-enqueue state when drain_empty_slab_queue() runs.
    ExpandableSlabAllocator alloc{kSmallSlab};

    constexpr int kNumSlabs = 4;
    std::vector<std::pair<int, void*>> allocations;
    allocations.reserve(kNumSlabs);

    for (int i = 0; i < kNumSlabs; ++i) {
        auto [slab_id, ptr] = alloc.allocate(kSmallSlab);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    EXPECT_EQ(alloc.slab_count(), kNumSlabs);

    // Deallocate all chunks from separate threads simultaneously.
    std::vector<std::thread> threads;
    threads.reserve(kNumSlabs);

    for (auto [slab_id, ptr] : allocations) {
        threads.emplace_back([&alloc, slab_id, ptr]() {
            alloc.deallocate(slab_id, ptr);
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // All threads are joined — all deallocate() calls have returned and all
    // Vyukov MPSC enqueue operations are complete. It is now safe for the
    // reactor thread to call allocate() and drain the empty slab queue.
    auto [slab_id_new, ptr_new] = alloc.allocate(64);
    EXPECT_NE(ptr_new, nullptr);
    alloc.deallocate(slab_id_new, ptr_new);
}

// ============================================================
// Stress: allocate/deallocate cycling
// ============================================================

TEST_F(ExpandableSlabAllocatorTest, StressAllocateDeallocateCycle) {
    // Repeatedly fill and drain the allocator to verify reclamation and
    // reuse work correctly over many cycles.
    ExpandableSlabAllocator alloc{kSmallSlab};

    constexpr int kCycles = 100;
    constexpr size_t kChunk = 64;
    const int chunks_per_slab = static_cast<int>(kSmallSlab / kChunk);

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::vector<std::pair<int, void*>> allocations;
        allocations.reserve(chunks_per_slab);

        for (int i = 0; i < chunks_per_slab; ++i) {
            auto [slab_id, ptr] = alloc.allocate(kChunk);
            ASSERT_NE(ptr, nullptr) << "cycle " << cycle << " allocation " << i << " failed";
            allocations.emplace_back(slab_id, ptr);
        }

        for (auto [slab_id, ptr] : allocations) {
            alloc.deallocate(slab_id, ptr);
        }

        // Trigger reclamation.
        auto [slab_id_probe, ptr_probe] = alloc.allocate(kChunk);
        ASSERT_NE(ptr_probe, nullptr) << "probe allocation failed at cycle " << cycle;
        alloc.deallocate(slab_id_probe, ptr_probe);
    }

    // Slab count should not have grown unboundedly — reclamation must have worked.
    // At most we'd expect a handful of slabs, not kCycles worth.
    EXPECT_LT(alloc.slab_count(), 10) << "slab count grew unexpectedly; reclamation may be broken";
}

} // namespace pubsub_itc_fw::tests
