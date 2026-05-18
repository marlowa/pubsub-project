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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/EmptySlabQueue.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/SlabAllocator.hpp>

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
            EXPECT_NE(allocations[i].second, allocations[j].second) << "duplicate pointer at indices " << i << " and " << j;
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
    EXPECT_EQ(slab_id_new, 0);        // reused, not a new slab
    EXPECT_EQ(alloc.slab_count(), 1); // no extra slab created

    alloc.deallocate(slab_id_new, ptr_new);
}

TEST_F(ExpandableSlabAllocatorTest, OldSlabIsDestroyedAfterChaining) {
    // Verifies the Vyukov sentinel reclamation lifecycle: a non-current slab
    // whose count reaches zero is enqueued, popped on the next drain, held as
    // the deferred sentinel until the drain AFTER it pops a successor (so
    // head_ has advanced past it), and only then destroyed.
    //
    // Using chunk_size == slab_size so each allocate() chains a fresh slab,
    // making each drain's GotItem deterministic.
    constexpr size_t slab_and_chunk = 64;
    ExpandableSlabAllocator alloc{slab_and_chunk};

    // Allocate from slab 0 (fills it).
    auto [slab_id_0, ptr_0] = alloc.allocate(slab_and_chunk);
    ASSERT_NE(ptr_0, nullptr);
    EXPECT_EQ(slab_id_0, 0);

    // Allocate again — chains slab 1; slab 0 cleared is_current_ but its
    // count is still 1, so no enqueue yet.
    auto [slab_id_1, ptr_1] = alloc.allocate(slab_and_chunk);
    ASSERT_NE(ptr_1, nullptr);
    EXPECT_EQ(slab_id_1, 1);
    EXPECT_EQ(alloc.slab_count(), 2);

    // Free slab-0's chunk — slab 0's count -> 0, slab 0 is enqueued.
    alloc.deallocate(slab_id_0, ptr_0);

    // Allocate again — drain pops slab 0 and HOLDS IT as the deferred
    // sentinel (not yet destroyed). The allocate then chains slab 2.
    auto [slab_id_2, ptr_2] = alloc.allocate(slab_and_chunk);
    ASSERT_NE(ptr_2, nullptr);
    EXPECT_EQ(slab_id_2, 2);
    EXPECT_EQ(alloc.slab_count(), 3);

    // Free slab-1's chunk — slab 1 enqueues.
    alloc.deallocate(slab_id_1, ptr_1);

    // Allocate again — drain pops slab 1 and head_ advances past slab 0.
    // The previously-deferred slab 0 is now safe to destroy and IS destroyed
    // in this drain. Slab 1 becomes the new deferred sentinel.
    auto [slab_id_3, ptr_3] = alloc.allocate(slab_and_chunk);
    ASSERT_NE(ptr_3, nullptr);

    // Slab count is at least 4 (more if reclamation has not yet been
    // reflected; the destroyed slot is still counted but is nullptr).
    EXPECT_GE(alloc.slab_count(), 4);

    // Slab 0 is now destroyed (nullptr in registry).
    EXPECT_THROW(alloc.deallocate(slab_id_0, ptr_0), PreconditionAssertion);

    alloc.deallocate(slab_id_2, ptr_2);
    alloc.deallocate(slab_id_3, ptr_3);
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
    // Verifies that once a slab has gone through the two-drain reclamation
    // lifecycle and been destroyed, deallocate against its slab_id throws.
    //
    // Using chunk_size == slab_size so each allocate() chains a fresh slab,
    // making each drain's GotItem deterministic.
    constexpr size_t slab_and_chunk = 64;
    ExpandableSlabAllocator alloc{slab_and_chunk};

    // Fill slab 0.
    auto [slab_id_0, ptr_0] = alloc.allocate(slab_and_chunk);

    // Chain slab 1.
    auto [slab_id_1, ptr_1] = alloc.allocate(slab_and_chunk);

    // Free slab 0's chunk — slab 0 enqueues.
    alloc.deallocate(slab_id_0, ptr_0);

    // Trigger drain 1: pops slab 0, holds it as deferred sentinel. Chains slab 2.
    auto [slab_id_2, ptr_2] = alloc.allocate(slab_and_chunk);

    // Free slab 1's chunk — slab 1 enqueues.
    alloc.deallocate(slab_id_1, ptr_1);

    // Trigger drain 2: pops slab 1; head_ advances past slab 0; slab 0 destroyed.
    auto [slab_id_3, ptr_3] = alloc.allocate(slab_and_chunk);

    // Slab 0 is now destroyed; deallocating against its slab_id must throw.
    EXPECT_THROW(alloc.deallocate(slab_id_0, ptr_0), PreconditionAssertion);

    alloc.deallocate(slab_id_2, ptr_2);
    alloc.deallocate(slab_id_3, ptr_3);
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
        threads.emplace_back([&alloc, slab_id, ptr]() { alloc.deallocate(slab_id, ptr); });
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

/*
 * Regression test for a hang discovered during the PduBurst integration soak.
 *
 * Production scenario: the reactor (one thread) allocates single-chunk PDU
 * payloads from the inbound slab allocator. The receiver application thread
 * (a different thread) deallocates each chunk after processing the PDU. With
 * the application thread keeping up with inbound traffic, each slab transitions
 * 0 -> 1 -> 0 -> 1 -> 0 ... while it is still the current slab.
 *
 * The hang requires a tight cross-thread interleaving: between the reactor's
 * first try_dequeue (which returns GotItem) and the reactor's follow-up
 * try_dequeue (which would return Empty and trigger reset_to_empty), the
 * application thread re-enqueues the same node. Because the queue's tail_
 * still points at that node from the previous enqueue, the second enqueue's
 * tail_.exchange(node) returns the node itself, and prev->next.store(node)
 * creates a self-loop. The reactor's follow-up try_dequeue then returns the
 * same node forever and the reactor never escapes the drain loop.
 *
 * This test runs two threads doing single-chunk allocate/deallocate cycles
 * for a fixed number of iterations. Without the fix it hangs the reactor
 * thread inside drain_empty_slab_queue. With it, both threads complete
 * within the time budget.
 *
 * The single-threaded variant of this scenario (one thread allocating and
 * deallocating in sequence) cannot trigger the bug because the consumer
 * always calls reset_to_empty between cycles before any re-enqueue can
 * happen. The bug needs a producer arriving in a specific window during
 * the consumer's drain.
 */
TEST_F(ExpandableSlabAllocatorTest, RepeatedSingleChunkAllocDeallocOfCurrentSlabDoesNotHang) {
    constexpr int cycles = 100'000;
    constexpr size_t chunk_size = 64;
    constexpr auto time_budget = std::chrono::seconds(10);

    auto workload = std::async(std::launch::async, [&]() -> bool {
        ExpandableSlabAllocator alloc{kSmallSlab};

        // Single-slot hand-off between the reactor (allocator) thread and the
        // deallocator thread. The reactor publishes a fresh allocation; the
        // deallocator picks it up and frees it. nullptr means "slot empty".
        std::atomic<void*> slot{nullptr};
        std::atomic<int> slot_slab_id{-1};
        std::atomic<bool> stop{false};

        std::thread deallocator([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                void* ptr = slot.exchange(nullptr, std::memory_order_acq_rel);
                if (ptr == nullptr)
                    continue;
                const int slab_id = slot_slab_id.load(std::memory_order_acquire);
                alloc.deallocate(slab_id, ptr);
            }
            // Drain any final outstanding allocation.
            void* ptr = slot.exchange(nullptr, std::memory_order_acq_rel);
            if (ptr != nullptr) {
                const int slab_id = slot_slab_id.load(std::memory_order_acquire);
                alloc.deallocate(slab_id, ptr);
            }
        });

        for (int i = 0; i < cycles; ++i) {
            // Wait for the deallocator to clear the previous slot.
            while (slot.load(std::memory_order_acquire) != nullptr) {
                std::this_thread::yield();
            }

            auto [slab_id, ptr] = alloc.allocate(chunk_size);
            if (ptr == nullptr) {
                stop.store(true, std::memory_order_release);
                deallocator.join();
                return false;
            }

            slot_slab_id.store(slab_id, std::memory_order_release);
            slot.store(ptr, std::memory_order_release);
        }

        // Wait for the deallocator to drain the final slot, then stop it.
        while (slot.load(std::memory_order_acquire) != nullptr) {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
        deallocator.join();
        return true;
    });

    const std::future_status status = workload.wait_for(time_budget);

    ASSERT_EQ(status, std::future_status::ready) << "ExpandableSlabAllocator hung during concurrent single-chunk alloc/dealloc cycles. "
                                                 << "This is the self-loop bug in EmptySlabQueue: the deallocator thread re-enqueued the "
                                                 << "current slab's node while the reactor thread was mid-drain, between its first GotItem "
                                                 << "and its second try_dequeue. The producer's tail_.exchange(node) found tail_ already "
                                                 << "equal to node, so node->next was set to node itself, and the reactor's next "
                                                 << "try_dequeue returned the same slab forever.";

    EXPECT_TRUE(workload.get()) << "allocate() returned nullptr unexpectedly during the workload";
}

// =============================================================================
// Stress workload helpers (used by ConcurrentAllocateAndDeallocateMakesProgress
// and ConcurrentSmallSlabHighChurn below).
//
// These tests reproduce the realistic call pattern of the framework: one
// "reactor" thread allocates chunks and hands them off to worker threads via a
// worker queue; the workers call deallocate. The slab allocator's empty-slab
// queue is therefore exercised by many concurrent producers (the workers, who
// reach SlabAllocator::deallocate -> EmptySlabQueue::enqueue when an
// outstanding count transitions to zero) and one consumer (the reactor, who
// reaches drain_empty_slab_queue from inside allocate).
//
// Small slab and chunk sizes force frequent slab switches and empty-slab
// notifications, which is what stresses the lock-free queue. The test runs
// for a bounded wall-clock duration and treats any exception thrown by the
// allocator on the reactor thread as a test failure -- including the
// drain-loop tripwire, which would indicate a genuine queue-state problem.
//
// Recommended invocation: build under Valgrind (USING_VALGRIND defined) and
// run with --gtest_repeat=50 (or more) to amplify the chance of catching
// timing-sensitive races.
// =============================================================================

namespace {

struct WorkItem {
    int slab_id{-1};
    void* ptr{nullptr};
};

// Thread-safe bounded queue used to hand off (slab_id, ptr) tuples from the
// reactor thread to the worker threads. Bounded so that runaway allocation
// can't OOM the test; workers block waiting for items, the reactor blocks
// waiting for space.
class WorkQueue {
  public:
    explicit WorkQueue(size_t capacity) : capacity_{capacity} {}

    void push(const WorkItem& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_full_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) {
            return;
        }
        items_.push_back(item);
        cv_not_empty_.notify_one();
    }

    bool pop(WorkItem& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) {
            return false;
        }
        out = items_.front();
        items_.pop_front();
        cv_not_full_.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    size_t size_unsafe() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<WorkItem> items_;
    size_t capacity_;
    bool closed_{false};
};

} // namespace

TEST_F(ExpandableSlabAllocatorTest, ConcurrentAllocateAndDeallocateMakesProgress) {
    // Small slab so chunks_per_slab is small (4-8 chunks). This forces frequent
    // slab switches, which is what exercises the empty-slab queue's enqueue
    // path. With a large slab the queue would barely be used.
    constexpr size_t slab_size = 512;
    constexpr size_t chunk_size = 64;
    constexpr int num_workers = 8;
    constexpr size_t worker_queue_capacity = 256;
    constexpr auto test_duration = std::chrono::seconds(5);

    ExpandableSlabAllocator allocator(slab_size);
    WorkQueue work_queue(worker_queue_capacity);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> allocations_completed{0};
    std::atomic<uint64_t> deallocations_completed{0};
    std::atomic<bool> reactor_threw{false};
    std::string reactor_exception_message;
    std::mutex reactor_exception_message_mutex;

    // Reactor thread: allocate chunks and post them to the worker queue.
    // If allocate() throws (e.g. the drain_empty_slab_queue tripwire), capture
    // the message and stop the test.
    std::thread reactor([&]() {
        try {
            const auto deadline = std::chrono::steady_clock::now() + test_duration;
            while (!stop.load(std::memory_order_relaxed)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    break;
                }
                auto [slab_id, ptr] = allocator.allocate(chunk_size);
                allocations_completed.fetch_add(1, std::memory_order_relaxed);
                work_queue.push({slab_id, ptr});
            }
        } catch (const std::exception& ex) {
            {
                std::lock_guard<std::mutex> lock(reactor_exception_message_mutex);
                reactor_exception_message = ex.what();
            }
            reactor_threw.store(true, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
        work_queue.close();
    });

    // Worker threads: pop work items and deallocate. These are the producers
    // on the slab's empty-slab queue: when their deallocate brings a slab's
    // outstanding count to zero, they enqueue the slab's node.
    std::vector<std::thread> workers;
    workers.reserve(num_workers);
    for (int worker_index = 0; worker_index < num_workers; ++worker_index) {
        workers.emplace_back([&]() {
            WorkItem item;
            while (work_queue.pop(item)) {
                allocator.deallocate(item.slab_id, item.ptr);
                deallocations_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    reactor.join();
    for (auto& worker : workers) {
        worker.join();
    }

    // Drain any remaining items the workers didn't get to (the queue may
    // still hold a few items if the workers were slow). We must drain them
    // before destroying the allocator so the slabs they belong to have a
    // chance to reach outstanding_count == 0 cleanly.
    {
        WorkItem item;
        while (work_queue.size_unsafe() > 0) {
            if (work_queue.pop(item)) {
                allocator.deallocate(item.slab_id, item.ptr);
                deallocations_completed.fetch_add(1, std::memory_order_relaxed);
            } else {
                break;
            }
        }
    }

    // Test outcome: the reactor must not have thrown. If it did, the most
    // likely cause is the drain_empty_slab_queue wall-clock tripwire firing,
    // which indicates the lock-free queue got into a state where the consumer
    // could not make progress -- exactly the bug we are trying to reproduce.
    if (reactor_threw.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(reactor_exception_message_mutex);
        FAIL() << "Reactor threw during stress test. Allocations completed: " << allocations_completed.load(std::memory_order_relaxed)
               << ", deallocations completed: " << deallocations_completed.load(std::memory_order_relaxed) << ". Exception: " << reactor_exception_message;
    }

    // Sanity check that the test actually exercised the allocator. If
    // allocations are trivially small, the workload below this is suspect.
    EXPECT_GT(allocations_completed.load(std::memory_order_relaxed), 1000U) << "Test workload was too small to exercise the allocator under load.";

    // Every allocation must be matched by a deallocation by the time we get
    // here, since the work queue has been fully drained.
    EXPECT_EQ(allocations_completed.load(std::memory_order_relaxed), deallocations_completed.load(std::memory_order_relaxed))
        << "Allocation/deallocation count mismatch indicates the test did not "
           "drain the work queue completely.";
}

TEST_F(ExpandableSlabAllocatorTest, ConcurrentSmallSlabHighChurn) {
    // Smaller slab and tinier chunks: chunks_per_slab is ~16, so slab switches
    // happen approximately every 16 allocations. With heavy load, that is
    // many switches per millisecond, maximising pressure on the empty-slab
    // queue.
    constexpr size_t slab_size = 1024;
    constexpr size_t chunk_size = 32;
    constexpr int num_workers = 16;
    constexpr size_t worker_queue_capacity = 512;
    constexpr auto test_duration = std::chrono::seconds(5);

    ExpandableSlabAllocator allocator(slab_size);
    WorkQueue work_queue(worker_queue_capacity);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> allocations_completed{0};
    std::atomic<uint64_t> deallocations_completed{0};
    std::atomic<bool> reactor_threw{false};
    std::string reactor_exception_message;
    std::mutex reactor_exception_message_mutex;

    std::thread reactor([&]() {
        try {
            const auto deadline = std::chrono::steady_clock::now() + test_duration;
            while (!stop.load(std::memory_order_relaxed)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    break;
                }
                auto [slab_id, ptr] = allocator.allocate(chunk_size);
                allocations_completed.fetch_add(1, std::memory_order_relaxed);
                work_queue.push({slab_id, ptr});
            }
        } catch (const std::exception& ex) {
            {
                std::lock_guard<std::mutex> lock(reactor_exception_message_mutex);
                reactor_exception_message = ex.what();
            }
            reactor_threw.store(true, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
        work_queue.close();
    });

    std::vector<std::thread> workers;
    workers.reserve(num_workers);
    for (int worker_index = 0; worker_index < num_workers; ++worker_index) {
        workers.emplace_back([&]() {
            WorkItem item;
            while (work_queue.pop(item)) {
                allocator.deallocate(item.slab_id, item.ptr);
                deallocations_completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    reactor.join();
    for (auto& worker : workers) {
        worker.join();
    }

    {
        WorkItem item;
        while (work_queue.size_unsafe() > 0) {
            if (work_queue.pop(item)) {
                allocator.deallocate(item.slab_id, item.ptr);
                deallocations_completed.fetch_add(1, std::memory_order_relaxed);
            } else {
                break;
            }
        }
    }

    if (reactor_threw.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(reactor_exception_message_mutex);
        FAIL() << "Reactor threw during stress test. Allocations completed: " << allocations_completed.load(std::memory_order_relaxed)
               << ", deallocations completed: " << deallocations_completed.load(std::memory_order_relaxed) << ". Exception: " << reactor_exception_message;
    }

    EXPECT_GT(allocations_completed.load(std::memory_order_relaxed), 1000U) << "Test workload was too small to exercise the allocator under load.";

    EXPECT_EQ(allocations_completed.load(std::memory_order_relaxed), deallocations_completed.load(std::memory_order_relaxed))
        << "Allocation/deallocation count mismatch indicates the test did not "
           "drain the work queue completely.";
}

// After an old slab has gone through the two-drain reclamation lifecycle and
// been destroyed, its registry slot is nullptr; a deallocate with that slab_id
// must throw. Using chunk_size == slab_size so each allocate() chains a fresh
// slab, making each drain's GotItem deterministic.
TEST_F(ExpandableSlabAllocatorTest, DestroyedSlabIdThrowsOnDeallocateCrossThread) {
    constexpr size_t slab_and_chunk = 64;
    ExpandableSlabAllocator allocator(slab_and_chunk);

    // Allocate from slab 0 (fills it).
    auto [id0, ptr0] = allocator.allocate(slab_and_chunk);
    ASSERT_EQ(id0, 0);
    ASSERT_NE(ptr0, nullptr);

    // Allocate again — chains slab 1.
    auto [id1, ptr1] = allocator.allocate(slab_and_chunk);
    ASSERT_EQ(id1, 1);
    ASSERT_NE(ptr1, nullptr);

    // Free slab 0 chunk from a background thread to exercise the cross-thread
    // deallocate path. Slab 0 is enqueued.
    std::thread t([&]() { allocator.deallocate(id0, ptr0); });
    t.join();

    // Drain 1: pops slab 0, defers it. Chains slab 2.
    auto [id2, ptr2] = allocator.allocate(slab_and_chunk);
    ASSERT_NE(ptr2, nullptr);

    // Free slab 1's chunk so slab 1 enqueues.
    allocator.deallocate(id1, ptr1);

    // Drain 2: pops slab 1; head_ advances past slab 0; slab 0 destroyed.
    auto [id3, ptr3] = allocator.allocate(slab_and_chunk);
    ASSERT_NE(ptr3, nullptr);

    // Slab 0 is now destroyed (nullptr in registry). Attempting to deallocate
    // with slab_id 0 must throw.
    int dummy = 0;
    EXPECT_THROW(allocator.deallocate(0, &dummy), PreconditionAssertion);

    allocator.deallocate(id2, ptr2);
    allocator.deallocate(id3, ptr3);
}

// Allocation of exactly slab_size forces a new slab on the very next allocation.
TEST_F(ExpandableSlabAllocatorTest, ExactSlabSizeAllocationForcesExpansion) {
    constexpr size_t adv_slab_size = 512;
    constexpr size_t adv_chunk_size = 64;
    ExpandableSlabAllocator allocator(adv_slab_size);

    auto [id0, ptr0] = allocator.allocate(adv_slab_size);
    EXPECT_EQ(id0, 0);
    EXPECT_NE(ptr0, nullptr);

    auto [id1, ptr1] = allocator.allocate(adv_chunk_size);
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
TEST_F(ExpandableSlabAllocatorTest, ConcurrentDeallocWhileReactorAllocates) {
    constexpr size_t adv_slab_size = 512;
    constexpr size_t adv_chunk_size = 64;
    constexpr int num_rounds = 20;
    constexpr int chunks_per_round = 8;

    ExpandableSlabAllocator allocator(adv_slab_size);

    for (int round = 0; round < num_rounds; ++round) {
        // Phase 1: reactor allocates a batch.
        std::vector<std::pair<int, void*>> allocations;
        allocations.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            auto [id, ptr] = allocator.allocate(adv_chunk_size);
            ASSERT_NE(ptr, nullptr);
            allocations.emplace_back(id, ptr);
        }

        // Phase 2: worker threads free the batch concurrently.
        std::vector<std::thread> threads;
        threads.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            threads.emplace_back([&allocator, &allocations, i]() { allocator.deallocate(allocations[i].first, allocations[i].second); });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Phase 3: reactor allocates again to trigger reclamation of the freed slabs.
        std::vector<std::pair<int, void*>> extra;
        for (int i = 0; i < 4; ++i) {
            auto [id, ptr] = allocator.allocate(adv_chunk_size);
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
TEST_F(ExpandableSlabAllocatorTest, NoStarvationUnderSustainedLoad) {
    constexpr size_t adv_slab_size = 512;
    constexpr size_t adv_chunk_size = 64;
    constexpr int num_threads = 8;
    constexpr int rounds = 50;
    constexpr int chunks_per_round = num_threads;

    ExpandableSlabAllocator allocator(adv_slab_size);
    std::atomic<int> errors{0};

    for (int round = 0; round < rounds; ++round) {
        // Reactor allocates one chunk per worker thread.
        std::vector<std::pair<int, void*>> allocations;
        allocations.reserve(chunks_per_round);

        for (int i = 0; i < chunks_per_round; ++i) {
            auto [id, ptr] = allocator.allocate(adv_chunk_size);
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
            workers.emplace_back([&allocator, id, ptr]() { allocator.deallocate(id, ptr); });
        }

        for (auto& t : workers) {
            t.join();
        }
        // All chunks freed — next round triggers reclamation via allocate().
    }

    EXPECT_EQ(errors.load(), 0) << "allocator returned nullptr under sustained load";
}

// =============================================================================
// Tests imported from the original SlabAllocatorTest.cpp's
// ExpandableSlabAllocatorTest section.
//
// References to that file's fixture constant `slab_size = 4096` have been
// replaced with kLargeSlab (= 65536) from this fixture; both are simply
// "generic large slab" sizes and the tests do not depend on the exact value.
// =============================================================================

TEST_F(ExpandableSlabAllocatorTest, BasicAllocation) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(slab_id, 0);
}

TEST_F(ExpandableSlabAllocatorTest, AllocationAndDeallocation) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_NE(ptr, nullptr);
    allocator.deallocate(slab_id, ptr);
}

TEST_F(ExpandableSlabAllocatorTest, SlabExpansionOnFull) {
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

TEST_F(ExpandableSlabAllocatorTest, ReclaimedCurrentSlabIsReset) {
    ExpandableSlabAllocator allocator(kLargeSlab);

    auto [slab_id, ptr] = allocator.allocate(64);
    EXPECT_EQ(slab_id, 0);

    allocator.deallocate(slab_id, ptr);

    // Under the new design, the next allocate self-reclaims the current
    // slab (resets its bump pointer because outstanding count is zero)
    // and then allocates from it again.
    auto [slab_id2, ptr2] = allocator.allocate(64);
    EXPECT_NE(ptr2, nullptr);
    EXPECT_EQ(slab_id2, 0);

    allocator.deallocate(slab_id2, ptr2);
}

TEST_F(ExpandableSlabAllocatorTest, OldEmptySlabIsDestroyed) {
    ExpandableSlabAllocator allocator(128);

    auto [id0, ptr0] = allocator.allocate(128);
    EXPECT_EQ(id0, 0);

    // Force expansion to slab 1
    auto [id1, ptr1] = allocator.allocate(64);
    EXPECT_EQ(id1, 1);
    EXPECT_EQ(allocator.slab_count(), 2);

    // Free the only chunk in slab 0 — notification enqueued
    allocator.deallocate(id0, ptr0);

    // Next allocation drains queue: slab 0 is not current (slab 1 is).
    // Under the Vyukov sentinel reclamation design, the drain pops slab 0 but
    // defers its destruction to the NEXT drain. This test only checks that
    // the allocation succeeds after the drain.
    auto [id2, ptr2] = allocator.allocate(64);
    EXPECT_NE(ptr2, nullptr);

    allocator.deallocate(id1, ptr1);
    allocator.deallocate(id2, ptr2);
}

TEST_F(ExpandableSlabAllocatorTest, SizeExceedsSlabSizeThrows) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    EXPECT_THROW(allocator.allocate(kLargeSlab + 1), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, AllocateZeroSizeThrows) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    EXPECT_THROW(allocator.allocate(0), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, DeallocateInvalidSlabIdThrows) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    int dummy = 0;
    EXPECT_THROW(allocator.deallocate(999, &dummy), PreconditionAssertion);
}

TEST_F(ExpandableSlabAllocatorTest, SlabSizeAccessor) {
    ExpandableSlabAllocator allocator(kLargeSlab);
    EXPECT_EQ(allocator.slab_size(), kLargeSlab);
}

TEST_F(ExpandableSlabAllocatorTest, MultipleAllocationsFromSameSlab) {
    ExpandableSlabAllocator allocator(kLargeSlab);
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

TEST_F(ExpandableSlabAllocatorTest, CrossThreadDeallocationsWithExpansion) {
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

} // namespace pubsub_itc_fw::tests
