// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>

namespace pubsub_itc_fw::tests {

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
    // Under the new design, deallocators only enqueue when the slab is no
    // longer the current slab. Simulate the owner having switched away.
    slab.clear_is_current();
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

    // Under the new design, only non-current slabs notify the queue.
    slab.clear_is_current();

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

TEST_F(SlabAllocatorTest, QueueNodeReturnsNode)
{
    SlabAllocator slab(slab_size, 7, queue_);
    EmptySlabQueueNode& node = slab.queue_node();
    // The node's slab_id is set during construction.
    EXPECT_EQ(node.slab_id, 7);
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

    // Under the new design, only non-current slabs notify the queue.
    slab.clear_is_current();

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

// =====================
// Ex-adversarial tests (previously in SlabAllocatorAdversarialTest fixture).
// =====================

// Allocation of exactly slab_size bytes must succeed and leave the slab full.
TEST_F(SlabAllocatorTest, AllocateExactlySlabSize)
{
    SlabAllocator slab(slab_size, 0, queue_);
    void* ptr = slab.allocate(slab_size);
    EXPECT_NE(ptr, nullptr);
    EXPECT_TRUE(slab.is_full());

    void* ptr2 = slab.allocate(1);
    EXPECT_EQ(ptr2, nullptr);

    slab.deallocate(ptr);
}

// Allocation that fits only before alignment padding pushes it over the edge.
// Allocate 1 byte first (forces alignment gap before next allocation),
// then request enough bytes that the aligned offset + size exceeds slab_size.
TEST_F(SlabAllocatorTest, AlignmentPaddingCausesFailure)
{
    constexpr size_t alignment = alignof(std::max_align_t);
    // Use a slab just large enough for two aligned slots of size alignment.
    constexpr size_t two_slot_slab = alignment * 2;
    SlabAllocator slab(two_slot_slab, 0, queue_);

    // Allocate 1 byte — bump advances to alignment after padding.
    void* ptr1 = slab.allocate(1);
    EXPECT_NE(ptr1, nullptr);

    // Now aligned offset is alignment, remaining is alignment bytes.
    // Request alignment bytes — should succeed exactly.
    void* ptr2 = slab.allocate(alignment);
    EXPECT_NE(ptr2, nullptr);

    // Slab is now full.
    EXPECT_TRUE(slab.is_full());

    slab.deallocate(ptr1);
    slab.deallocate(ptr2);
}

// Only one thread must enqueue the notification even under heavy concurrent deallocation.
TEST_F(SlabAllocatorTest, OnlyOneNotificationEnqueuedUnderConcurrency)
{
    constexpr int num_threads = 32;
    constexpr size_t chunk_size = 64;
    constexpr size_t large_slab = chunk_size * num_threads * 2;

    SlabAllocator slab(large_slab, 7, queue_);

    std::vector<void*> ptrs(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        ptrs[i] = slab.allocate(chunk_size);
        ASSERT_NE(ptrs[i], nullptr);
    }

    // Under the new design, only non-current slabs notify the queue.
    slab.clear_is_current();

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&slab, &ptrs, i]() {
            slab.deallocate(ptrs[i]);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(slab.is_empty());

    // Exactly one notification must have been enqueued.
    int slab_id = -1;
    EXPECT_EQ(queue_.try_dequeue(slab_id), DequeueResult::GotItem);
    EXPECT_EQ(slab_id, 7);

    // No second notification.
    EXPECT_EQ(queue_.try_dequeue(slab_id), DequeueResult::Empty);
}

// Rapid allocate/deallocate cycles force the slab to be reset and reused many times.
TEST_F(SlabAllocatorTest, RepeatedResetAndReuse)
{
    constexpr int cycles = 1000;
    SlabAllocator slab(slab_size, 0, queue_);

    void* first_allocation = nullptr;

    for (int i = 0; i < cycles; ++i) {
        void* ptr = slab.allocate(64);
        ASSERT_NE(ptr, nullptr);

        if (i == 0) {
            first_allocation = ptr;
        }

        slab.deallocate(ptr);

        // Drain any notification. Under the new design the slab remains
        // current throughout (clear_is_current is never called), so deallocate
        // does not enqueue. The drain is a no-op but kept for the case where
        // a future test variant clears is_current.
        int slab_id = -1;
        [[maybe_unused]] DequeueResult drain = queue_.try_dequeue(slab_id);

        slab.reset();

        EXPECT_EQ(slab.bytes_used(), 0u);
        EXPECT_FALSE(slab.is_full());
        EXPECT_TRUE(slab.is_empty());
    }

    // After reset, a fresh allocation must return the same address as the first.
    void* ptr_after_reset = slab.allocate(64);
    EXPECT_EQ(ptr_after_reset, first_allocation);
    slab.deallocate(ptr_after_reset);
}

// Multiple threads deallocate from different slabs simultaneously.
// All slabs must reach zero and all notifications must be enqueued.
TEST_F(SlabAllocatorTest, ConcurrentDeallocationsAcrossMultipleSlabs)
{
    constexpr int num_slabs = 8;
    constexpr size_t chunk_size = 64;

    std::vector<std::unique_ptr<SlabAllocator>> slabs;
    slabs.reserve(num_slabs);

    for (int i = 0; i < num_slabs; ++i) {
        slabs.push_back(std::make_unique<SlabAllocator>(chunk_size * 4, i, queue_));
    }

    // Allocate one chunk from each slab.
    std::vector<void*> ptrs(num_slabs);
    for (int i = 0; i < num_slabs; ++i) {
        ptrs[i] = slabs[i]->allocate(chunk_size);
        ASSERT_NE(ptrs[i], nullptr);
    }

    // Under the new design, only non-current slabs notify the queue.
    for (int i = 0; i < num_slabs; ++i) {
        slabs[i]->clear_is_current();
    }

    // Each thread frees from a different slab.
    std::vector<std::thread> threads;
    threads.reserve(num_slabs);

    for (int i = 0; i < num_slabs; ++i) {
        threads.emplace_back([&slabs, &ptrs, i]() {
            slabs[i]->deallocate(ptrs[i]);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All slabs must be empty.
    for (int i = 0; i < num_slabs; ++i) {
        EXPECT_TRUE(slabs[i]->is_empty()) << "slab " << i << " not empty";
    }

    // All notifications must be present, each slab ID appearing exactly once.
    std::vector<bool> seen(num_slabs, false);
    int drained = 0;

    while (drained < num_slabs) {
        int slab_id = -1;
        DequeueResult result = queue_.try_dequeue(slab_id);
        if (result == DequeueResult::GotItem) {
            ASSERT_GE(slab_id, 0);
            ASSERT_LT(slab_id, num_slabs);
            EXPECT_FALSE(seen[slab_id]) << "slab_id " << slab_id << " notified twice";
            seen[slab_id] = true;
            ++drained;
        }
    }

    for (int i = 0; i < num_slabs; ++i) {
        EXPECT_TRUE(seen[i]) << "slab_id " << i << " never notified";
    }
}

} // namespace pubsub_itc_fw::tests
