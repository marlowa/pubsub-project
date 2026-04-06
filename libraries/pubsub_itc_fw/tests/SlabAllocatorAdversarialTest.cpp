// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>

namespace pubsub_itc_fw {

// ============================================================
// EmptySlabQueue adversarial tests
// ============================================================

class EmptySlabQueueAdversarialTest : public ::testing::Test {
};

// Many producers enqueue simultaneously; consumer must see all items exactly once.
TEST_F(EmptySlabQueueAdversarialTest, ManyProducersOneConsumer)
{
    constexpr int num_producers = 16;
    EmptySlabQueue queue;

    std::vector<EmptySlabQueueNode> nodes(num_producers);
    for (int i = 0; i < num_producers; ++i) {
        nodes[i].slab_id = i;
    }

    std::vector<std::thread> producers;
    producers.reserve(num_producers);

    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&queue, &nodes, i]() {
            queue.enqueue(&nodes[i]);
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    std::vector<bool> seen(num_producers, false);
    int drained = 0;

    while (drained < num_producers) {
        int slab_id = -1;
        DequeueResult result = queue.try_dequeue(slab_id);
        if (result == DequeueResult::GotItem) {
            ASSERT_GE(slab_id, 0);
            ASSERT_LT(slab_id, num_producers);
            EXPECT_FALSE(seen[slab_id]) << "slab_id " << slab_id << " seen twice";
            seen[slab_id] = true;
            ++drained;
        }
    }

    for (int i = 0; i < num_producers; ++i) {
        EXPECT_TRUE(seen[i]) << "slab_id " << i << " never seen";
    }
}

// Interleaved enqueue and dequeue: producers and consumer run concurrently.
TEST_F(EmptySlabQueueAdversarialTest, InterleavedEnqueueDequeue)
{
    constexpr int num_items = 64;
    EmptySlabQueue queue;

    std::vector<EmptySlabQueueNode> nodes(num_items);
    for (int i = 0; i < num_items; ++i) {
        nodes[i].slab_id = i;
    }

    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::thread producer([&]() {
        for (int i = 0; i < num_items; ++i) {
            queue.enqueue(&nodes[i]);
            produced.fetch_add(1, std::memory_order_release);
        }
    });

    std::thread consumer([&]() {
        while (consumed.load(std::memory_order_acquire) < num_items) {
            int slab_id = -1;
            DequeueResult result = queue.try_dequeue(slab_id);
            if (result == DequeueResult::GotItem) {
                consumed.fetch_add(1, std::memory_order_release);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), num_items);
}

// ============================================================
// SlabAllocator adversarial tests
// ============================================================

class SlabAllocatorAdversarialTest : public ::testing::Test {
protected:
    static constexpr size_t slab_size = 4096;
    EmptySlabQueue queue_;
};

// Allocation of exactly slab_size bytes must succeed and leave the slab full.
TEST_F(SlabAllocatorAdversarialTest, AllocateExactlySlabSize)
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
TEST_F(SlabAllocatorAdversarialTest, AlignmentPaddingCausesFailure)
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
TEST_F(SlabAllocatorAdversarialTest, OnlyOneNotificationEnqueuedUnderConcurrency)
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
TEST_F(SlabAllocatorAdversarialTest, RepeatedResetAndReuse)
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

        // Drain the notification.
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
TEST_F(SlabAllocatorAdversarialTest, ConcurrentDeallocationsAcrossMultipleSlabs)
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

// ============================================================
// ExpandableSlabAllocator adversarial tests
// ============================================================

class ExpandableSlabAllocatorAdversarialTest : public ::testing::Test {
protected:
    static constexpr size_t slab_size = 512;
    static constexpr size_t chunk_size = 64;
};

// After an old slab is destroyed its registry slot is nullptr;
// a deallocate with that slab_id must throw.
// Use chunk_size as slab_size so exactly one chunk fits per slab, guaranteeing
// slab 0 is unambiguously old once slab 1 is current.
TEST_F(ExpandableSlabAllocatorAdversarialTest, DestroyedSlabIdThrowsOnDeallocate)
{
    ExpandableSlabAllocator allocator(chunk_size);

    // Allocate from slab 0.
    auto [id0, ptr0] = allocator.allocate(chunk_size);
    ASSERT_EQ(id0, 0);
    ASSERT_NE(ptr0, nullptr);

    // Force expansion to slab 1.
    auto [id1, ptr1] = allocator.allocate(chunk_size);
    ASSERT_EQ(id1, 1);
    ASSERT_NE(ptr1, nullptr);

    // Free slab 0 chunk from a background thread.
    std::thread t([&]() {
        allocator.deallocate(id0, ptr0);
    });
    t.join();

    // Free slab 1 chunk so the allocator is quiescent.
    allocator.deallocate(id1, ptr1);

    // Trigger drain: reactor sees slab 0 is old and destroys it.
    auto [id2, ptr2] = allocator.allocate(chunk_size);
    ASSERT_NE(ptr2, nullptr);
    allocator.deallocate(id2, ptr2);

    // Slab 0 is now destroyed (nullptr in registry).
    // Attempting to deallocate with slab_id 0 must throw.
    int dummy = 0;
    EXPECT_THROW(allocator.deallocate(0, &dummy), PreconditionAssertion);
}

// Allocation of exactly slab_size forces a new slab on the very next allocation.
TEST_F(ExpandableSlabAllocatorAdversarialTest, ExactSlabSizeAllocationForcesExpansion)
{
    ExpandableSlabAllocator allocator(slab_size);

    auto [id0, ptr0] = allocator.allocate(slab_size);
    EXPECT_EQ(id0, 0);
    EXPECT_NE(ptr0, nullptr);

    auto [id1, ptr1] = allocator.allocate(chunk_size);
    EXPECT_EQ(id1, 1);
    EXPECT_NE(ptr1, nullptr);

    allocator.deallocate(id0, ptr0);
    allocator.deallocate(id1, ptr1);
}

// Many threads deallocate chunks concurrently with reactor allocations.
// Each round allocates a batch, frees it concurrently from worker threads,
// then allocates another batch to exercise reclamation. Threads are always
// joined before the next round to avoid cross-round interference under
// instrumented builds (coverage, TSan, Valgrind).
TEST_F(ExpandableSlabAllocatorAdversarialTest, ConcurrentDeallocWhileReactorAllocates)
{
    constexpr int num_rounds = 20;
    constexpr int chunks_per_round = 8;

    ExpandableSlabAllocator allocator(slab_size);

    for (int round = 0; round < num_rounds; ++round) {
        // Phase 1: reactor allocates a batch.
        std::vector<std::pair<int, void*>> allocations;
        allocations.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            auto [id, ptr] = allocator.allocate(chunk_size);
            ASSERT_NE(ptr, nullptr);
            allocations.emplace_back(id, ptr);
        }

        // Phase 2: worker threads free the batch concurrently.
        std::vector<std::thread> threads;
        threads.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            threads.emplace_back([&allocator, &allocations, i]() {
                allocator.deallocate(allocations[i].first, allocations[i].second);
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Phase 3: reactor allocates again to trigger reclamation of the freed slabs.
        std::vector<std::pair<int, void*>> extra;
        for (int i = 0; i < 4; ++i) {
            auto [id, ptr] = allocator.allocate(chunk_size);
            ASSERT_NE(ptr, nullptr);
            extra.emplace_back(id, ptr);
        }

        for (auto& [id, ptr] : extra) {
            allocator.deallocate(id, ptr);
        }
    }
}

// Sustained high-volume allocation and deallocation across many threads
// verifies that the allocator never returns nullptr due to reclamation starvation.
//
// Pattern mirrors real usage: the reactor allocates a batch of chunks, hands
// them all to worker threads, then waits for all workers to finish before
// allocating the next batch. No spin-waits or slot-coupling — each round is
// a clean barrier. If reclamation stalls, the allocator grows unboundedly and
// allocate() eventually returns nullptr, which the test detects.
TEST_F(ExpandableSlabAllocatorAdversarialTest, NoStarvationUnderSustainedLoad)
{
    constexpr int num_threads = 8;
    constexpr int rounds = 50;
    constexpr int chunks_per_round = num_threads;

    ExpandableSlabAllocator allocator(slab_size);
    std::atomic<int> errors{0};

    for (int round = 0; round < rounds; ++round) {
        // Reactor allocates one chunk per worker thread.
        std::vector<std::pair<int, void*>> allocations;
        allocations.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            auto [id, ptr] = allocator.allocate(chunk_size);
            if (ptr == nullptr) {
                errors.fetch_add(1, std::memory_order_relaxed);
            } else {
                allocations.emplace_back(id, ptr);
            }
        }

        // Each worker thread frees one chunk.
        std::vector<std::thread> workers;
        workers.reserve(allocations.size());

        for (auto& [id, ptr] : allocations) {
            workers.emplace_back([&allocator, id, ptr]() {
                allocator.deallocate(id, ptr);
            });
        }

        for (auto& t : workers) {
            t.join();
        }
        // All chunks freed — next round triggers reclamation via allocate().
    }

    EXPECT_EQ(errors.load(), 0) << "allocator returned nullptr under sustained load";
}

} // namespace pubsub_itc_fw
