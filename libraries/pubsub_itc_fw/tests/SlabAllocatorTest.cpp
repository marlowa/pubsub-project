// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>
#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

// ============================================================
// EmptySlabQueue tests
// ============================================================

class EmptySlabQueueTest : public ::testing::Test {
};

TEST_F(EmptySlabQueueTest, DequeueOnEmptyQueueReturnsEmpty)
{
    EmptySlabQueue queue;
    int slab_id = -1;
    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::Empty);
}

TEST_F(EmptySlabQueueTest, EnqueueThenDequeueReturnsItem)
{
    EmptySlabQueue queue;
    EmptySlabQueueNode node;
    node.slab_id = 42;

    queue.enqueue(&node);

    int slab_id = -1;
    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 42);
}

TEST_F(EmptySlabQueueTest, DequeueAfterDrainReturnsEmpty)
{
    EmptySlabQueue queue;
    EmptySlabQueueNode node;
    node.slab_id = 7;

    queue.enqueue(&node);

    int slab_id = -1;
    [[maybe_unused]] DequeueResult first = queue.try_dequeue(slab_id);

    DequeueResult result = queue.try_dequeue(slab_id);
    EXPECT_EQ(result, DequeueResult::Empty);
}

TEST_F(EmptySlabQueueTest, MultipleEnqueueDequeueInOrder)
{
    EmptySlabQueue queue;
    EmptySlabQueueNode node0;
    EmptySlabQueueNode node1;
    EmptySlabQueueNode node2;
    node0.slab_id = 0;
    node1.slab_id = 1;
    node2.slab_id = 2;

    queue.enqueue(&node0);
    queue.enqueue(&node1);
    queue.enqueue(&node2);

    int slab_id = -1;

    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 0);

    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 1);

    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 2);

    EXPECT_EQ(queue.try_dequeue(slab_id), DequeueResult::Empty);
}

TEST_F(EmptySlabQueueTest, EnqueueNullptrThrows)
{
    EmptySlabQueue queue;
    EXPECT_THROW(queue.enqueue(nullptr), PreconditionAssertion);
}

// ============================================================
// SlabAllocator tests
// ============================================================

class SlabAllocatorTest : public ::testing::Test {
protected:
    static constexpr size_t slab_size = 4096;
    EmptySlabQueue queue_;
};

TEST_F(SlabAllocatorTest, BasicAllocation)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr = slab.allocate(64);
    EXPECT_NE(ptr, nullptr);
}

TEST_F(SlabAllocatorTest, AllocationFailsWhenFull)
{
    SlabAllocator slab(64, 0, queue_);
    void* ptr = slab.allocate(64);
    EXPECT_NE(ptr, nullptr);
    void* ptr2 = slab.allocate(1);
    EXPECT_EQ(ptr2, nullptr);
}

TEST_F(SlabAllocatorTest, ContainsReturnsTrueForAllocatedPtr)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr = slab.allocate(64);
    EXPECT_TRUE(slab.contains(ptr));
}

TEST_F(SlabAllocatorTest, ContainsReturnsFalseForForeignPtr)
{
    SlabAllocator slab(slab_size, 0, queue_);
    int local = 0;
    EXPECT_FALSE(slab.contains(&local));
}

TEST_F(SlabAllocatorTest, IsEmptyAfterDeallocatingSingleChunk)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr = slab.allocate(64);
    EXPECT_FALSE(slab.is_empty());
    slab.deallocate(ptr);
    EXPECT_TRUE(slab.is_empty());
}

TEST_F(SlabAllocatorTest, EmptySlabQueueNotifiedWhenLastChunkFreed)
{
    SlabAllocator slab(slab_size, 5, queue_);
    void* ptr = slab.allocate(64);
    slab.deallocate(ptr);

    int slab_id = -1;
    DequeueResult result = queue_.try_dequeue(slab_id);
    EXPECT_EQ(result, DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 5);
}

TEST_F(SlabAllocatorTest, QueueNotNotifiedUntilLastChunkFreed)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr1 = slab.allocate(64);
    void* ptr2 = slab.allocate(64);

    slab.deallocate(ptr1);

    int slab_id = -1;
    EXPECT_EQ(queue_.try_dequeue(slab_id), DequeueResult::Empty);

    slab.deallocate(ptr2);

    EXPECT_EQ(queue_.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 0);
}

TEST_F(SlabAllocatorTest, ResetAllowsReuse)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr = slab.allocate(64);
    slab.deallocate(ptr);

    // Drain notification
    int slab_id = -1;
    [[maybe_unused]] DequeueResult drain_result = queue_.try_dequeue(slab_id);

    slab.reset();
    EXPECT_TRUE(slab.is_empty());
    EXPECT_FALSE(slab.is_full());

    void* ptr2 = slab.allocate(64);
    EXPECT_NE(ptr2, nullptr);
}

TEST_F(SlabAllocatorTest, AllocateZeroSizeThrows)
{
    SlabAllocator slab(slab_size, 0, queue_);
    EXPECT_THROW(
        { [[maybe_unused]] void* p = slab.allocate(0); },
        PreconditionAssertion);
}

TEST_F(SlabAllocatorTest, DeallocateNullptrThrows)
{
    SlabAllocator slab(slab_size, 0, queue_);
    EXPECT_THROW(slab.deallocate(nullptr), PreconditionAssertion);
}

TEST_F(SlabAllocatorTest, SlabIdIsCorrect)
{
    SlabAllocator slab(slab_size, 99, queue_);
    EXPECT_EQ(slab.slab_id(), 99);
}

TEST_F(SlabAllocatorTest, CapacityMatchesSlabSize)
{
    SlabAllocator slab(slab_size, 0, queue_);
    EXPECT_EQ(slab.capacity(), slab_size);
}

TEST_F(SlabAllocatorTest, AllocationsAreAligned)
{
    SlabAllocator slab(slab_size, 0, queue_);
    constexpr size_t alignment = alignof(std::max_align_t);

    void* ptr1 = slab.allocate(1);
    void* ptr2 = slab.allocate(1);

    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr1) % alignment, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr2) % alignment, 0u);
}

// Cross-thread deallocation: multiple threads free chunks from the same slab.
TEST_F(SlabAllocatorTest, CrossThreadDeallocation)
{
    constexpr int num_threads = 8;
    constexpr int chunks_per_thread = 16;
    constexpr size_t chunk_size = 64;
    constexpr size_t large_slab = chunk_size * num_threads * chunks_per_thread * 2;

    SlabAllocator slab(large_slab, 0, queue_);

    std::vector<void*> ptrs(num_threads * chunks_per_thread);

    for (int i = 0; i < num_threads * chunks_per_thread; ++i) {
        ptrs[i] = slab.allocate(chunk_size);
        ASSERT_NE(ptrs[i], nullptr);
    }

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&slab, &ptrs, t]() {
            for (int i = 0; i < chunks_per_thread; ++i) {
                slab.deallocate(ptrs[t * chunks_per_thread + i]);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(slab.is_empty());

    int slab_id = -1;
    EXPECT_EQ(queue_.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 0);
}

// ============================================================
// ExpandableSlabAllocator tests
// ============================================================

class ExpandableSlabAllocatorTest : public ::testing::Test {
protected:
    static constexpr size_t slab_size = 4096;
};

TEST_F(ExpandableSlabAllocatorTest, BasicAllocation)
{
    ExpandableSlabAllocator allocator(slab_size);
    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(slab_id, 0);
}

TEST_F(ExpandableSlabAllocatorTest, AllocationAndDeallocation)
{
    ExpandableSlabAllocator allocator(slab_size);
    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_NE(ptr, nullptr);
    allocator.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, SlabExpansionOnFull)
{
    ExpandableSlabAllocator allocator(128);

    // Fill the first slab
    auto [id1, ptr1] = allocator.allocate(128);
    EXPECT_NE(ptr1, nullptr);
    EXPECT_EQ(id1, 0);

    // This should trigger expansion
    auto [id2, ptr2] = allocator.allocate(64);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_EQ(id2, 1);

    allocator.deallocate(id1, ptr1);
    allocator.deallocate(id2, ptr2);
}

TEST_F(ExpandableSlabAllocatorTest, ReclaimedCurrentSlabIsReset)
{
    ExpandableSlabAllocator allocator(slab_size);

    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_EQ(slab_id, 0);

    allocator.deallocate(slab_id, ptr);

    // Next allocation should drain the queue and reset slab 0, then allocate from it
    auto [slab_id2, ptr2] = allocator.allocate(64);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_EQ(slab_id2, 0);

    allocator.deallocate(slab_id2, ptr2);
}

TEST_F(ExpandableSlabAllocatorTest, OldEmptySlabIsDestroyed)
{
    ExpandableSlabAllocator allocator(128);

    auto [id0, ptr0] = allocator.allocate(128);
    EXPECT_EQ(id0, 0);

    // Force expansion to slab 1
    auto [id1, ptr1] = allocator.allocate(64);
    EXPECT_EQ(id1, 1);
    EXPECT_EQ(allocator.slab_count(), 2);

    // Free the only chunk in slab 0 — notification enqueued
    allocator.deallocate(id0, ptr0);

    // Next allocation drains queue: slab 0 is not current (slab 1 is), so destroy it
    auto [id2, ptr2] = allocator.allocate(64);
    EXPECT_NE(ptr2, nullptr);

    allocator.deallocate(id1, ptr1);
    allocator.deallocate(id2, ptr2);
}

TEST_F(ExpandableSlabAllocatorTest, SizeExceedsSlabSizeThrows)
{
    ExpandableSlabAllocator allocator(slab_size);
    EXPECT_THROW(allocator.allocate(slab_size + 1), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, AllocateZeroSizeThrows)
{
    ExpandableSlabAllocator allocator(slab_size);
    EXPECT_THROW(allocator.allocate(0), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateNullptrThrows)
{
    ExpandableSlabAllocator allocator(slab_size);
    EXPECT_THROW(allocator.deallocate(0, nullptr), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateInvalidSlabIdThrows)
{
    ExpandableSlabAllocator allocator(slab_size);
    int dummy = 0;
    EXPECT_THROW(allocator.deallocate(999, &dummy), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, SlabSizeAccessor)
{
    ExpandableSlabAllocator allocator(slab_size);
    EXPECT_EQ(allocator.slab_size(), slab_size);
}

TEST_F(ExpandableSlabAllocatorTest, MultipleAllocationsFromSameSlab)
{
    ExpandableSlabAllocator allocator(slab_size);
    constexpr int count = 8;
    std::vector<std::pair<int, void*>> allocations;
    allocations.reserve(count);

    for (int i = 0; i < count; ++i) {
        auto [slab_id, ptr] = allocator.allocate(64);
        EXPECT_NE(ptr, nullptr);
        EXPECT_EQ(slab_id, 0);
        allocations.emplace_back(slab_id, ptr);
    }

    for (auto& [slab_id, ptr] : allocations) {
        allocator.deallocate(slab_id, ptr);
    }
}

TEST_F(ExpandableSlabAllocatorTest, CrossThreadDeallocationsWithExpansion)
{
    constexpr size_t small_slab = 512;
    constexpr size_t chunk_size = 64;
    constexpr int num_threads = 4;
    constexpr int chunks_per_thread = 4;

    ExpandableSlabAllocator allocator(small_slab);

    std::vector<std::pair<int, void*>> allocations;
    allocations.reserve(num_threads * chunks_per_thread);

    for (int i = 0; i < num_threads * chunks_per_thread; ++i) {
        auto [slab_id, ptr] = allocator.allocate(chunk_size);
        ASSERT_NE(ptr, nullptr);
        allocations.emplace_back(slab_id, ptr);
    }

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&allocator, &allocations, t]() {
            for (int i = 0; i < chunks_per_thread; ++i) {
                auto& [slab_id, ptr] = allocations[t * chunks_per_thread + i];
                allocator.deallocate(slab_id, ptr);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

} // namespace pubsub_itc_fw
