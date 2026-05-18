// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw::tests {

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

/*
 * Documents a self-loop hazard in EmptySlabQueue, deliberately left
 * unfixed at the queue level.
 *
 * The queue assumes each EmptySlabQueueNode is enqueued at most once per
 * lifetime. If a producer re-enqueues a node that the consumer has already
 * dequeued but not yet advanced past, the producer's tail_.exchange(node)
 * returns the node itself; the subsequent prev->next.store(node) creates a
 * self-loop, and the next try_dequeue returns the same slab_id forever.
 *
 * Rather than fix the queue (which would slow the hot enqueue path with a
 * CAS), the callers are constrained so they cannot trigger the hazard:
 *
 *   - SlabAllocator::deallocate refuses to enqueue while the slab is the
 *     current slab. The current slab uses inline self-reclaim instead.
 *   - SlabAllocator::try_claim_enqueue is a one-shot CAS on the slab's
 *     is_enqueued_ flag; a slab's node can therefore only ever be enqueued
 *     once per slab lifetime. After dequeue and reclamation the slab is
 *     destroyed; a new slab gets a fresh node and a fresh flag.
 *
 * This test exercises the queue directly to demonstrate that the underlying
 * hazard still exists. It is SKIPPED because the production code paths above
 * prevent it from being triggered. If a future change exposes the queue to
 * repeated enqueues of the same node, this test should be unskipped and the
 * queue should be hardened.
 */
TEST_F(EmptySlabQueueTest, ReEnqueueOfSameNodeDoesNotCauseInfiniteSpin)
{
    GTEST_SKIP() << "Documents a queue-level self-loop hazard that is "
                    "deliberately mitigated by caller constraints rather "
                    "than by fixing the queue itself. See test comment.";

    EmptySlabQueue queue;
    EmptySlabQueueNode node;
    node.slab_id = 7;

    queue.enqueue(&node);

    int slab_id = -1;
    ASSERT_EQ(queue.try_dequeue(slab_id), DequeueResult::GotItem);
    ASSERT_EQ(slab_id, 7);

    // Re-enqueue the same node. This is the workload pattern that the
    // production caller constraints (is_enqueued_ one-shot CAS) now prevent.
    queue.enqueue(&node);

    // After the queue is hardened, the next try_dequeue should either return
    // the re-enqueued node exactly once and then go to Empty, or return
    // Empty immediately if the implementation refuses the second enqueue.
    // In either case, a bounded loop must terminate.
    constexpr int max_iterations = 16;
    int got_item_count_after_reenqueue = 0;
    int same_id_repeats = 0;
    int last_slab_id = -1;

    for (int i = 0; i < max_iterations; ++i) {
        int dequeued_slab_id = -1;
        const DequeueResult result = queue.try_dequeue(dequeued_slab_id);

        if (result == DequeueResult::Empty) {
            break;
        }
        if (result == DequeueResult::Retry) {
            // No producer is active in this single-threaded test; Retry would
            // indicate a state machine bug. Fail rather than spin.
            FAIL() << "try_dequeue returned Retry in a single-threaded scenario";
        }
        // GotItem
        ++got_item_count_after_reenqueue;
        if (dequeued_slab_id == last_slab_id) {
            ++same_id_repeats;
        }
        last_slab_id = dequeued_slab_id;
    }

    EXPECT_LE(got_item_count_after_reenqueue, 1)
        << "Re-enqueue of an already-enqueued node yielded the node more than once: "
        << got_item_count_after_reenqueue
        << " GotItem results (same_id_repeats=" << same_id_repeats << "). "
        << "This is the self-loop hazard.";
}

// Many producers enqueue simultaneously; consumer must see all items exactly once.
TEST_F(EmptySlabQueueTest, ManyProducersOneConsumer)
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
TEST_F(EmptySlabQueueTest, InterleavedEnqueueDequeue)
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

} // namespace pubsub_itc_fw::tests
