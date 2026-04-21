// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm> // For std::find
#include <atomic>    // For std::atomic

#include <chrono>
#include <functional> // For std::function
#include <iostream>   // for cout in statistics
#include <memory>     // For std::unique_ptr
#include <mutex>
#include <random> // For std::shuffle
#include <set>
#include <string> // For std::string
#include <thread> // For std::thread
#include <unordered_set>
#include <utility>
#include <vector> // For std::vector

#include <cstddef> // for size_t
#include <cstdint>

#include <gtest/gtest.h> // Google Test framework

#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>
#include <pubsub_itc_fw/tests_common/LatencyRecorder.hpp>

namespace pubsub_itc_fw::tests {
struct TestObject;
}

namespace {
// ---------------------------------------------------------------------------
// Helper: verify all pointers are unique and non-null
// ---------------------------------------------------------------------------
void expect_unique_non_null(const std::vector<pubsub_itc_fw::tests::TestObject*>& ptrs, const char* context) {
    for (size_t i = 0; i < ptrs.size(); ++i) {
        ASSERT_NE(ptrs[i], nullptr) << context << ": nullptr at index " << i;
    }
    std::unordered_set<pubsub_itc_fw::tests::TestObject*> seen;
    for (auto* p : ptrs) {
        EXPECT_TRUE(seen.insert(p).second) << context << ": duplicate pointer " << p;
    }
}

} // unnamed namespace

namespace pubsub_itc_fw::tests {

// --- Test Object Definition ---
// A simple test object to be managed by the pool.
// It tracks its construction/destruction and ensures it's large enough for the intrusive free list.
struct TestObject {
    int id_ = 0; // Instance-specific ID
    // Pad with data to ensure it's larger than FixedSizeMemoryPool::FreeListNode (sizeof(std::atomic<T*>))
    std::byte padding_[128]; // Ensure sufficient size for intrusive linking

    // Static counters to track allocations and deallocations across all instances
    static std::atomic<int> s_constructor_count;
    static std::atomic<int> s_destructor_count;

    TestObject() {
        s_constructor_count++;
    }

    ~TestObject() {
        s_destructor_count++;
    }

    // Reset static counters before each test case
    static void reset_counts() {
        s_constructor_count = 0;
        s_destructor_count = 0;
    }
};

// Initialize static members
std::atomic<int> TestObject::s_constructor_count(0);
std::atomic<int> TestObject::s_destructor_count(0);

class ExpandablePoolAllocatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        TestObject::reset_counts();
        pool_exhausted_callback_count_ = 0;
        invalid_free_callback_count_ = 0;
        huge_pages_error_callback_count_ = 0;

        // Corrected: Pass FwLogLevel::Info to the constructor of UnitTestLogger
        unit_test_logger_ = std::make_unique<pubsub_itc_fw::QuillLogger>();
    }

    std::atomic<int> pool_exhausted_callback_count_;
    std::atomic<int> invalid_free_callback_count_;
    std::atomic<int> huge_pages_error_callback_count_;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> unit_test_logger_;

    // Callbacks for the allocator to use
    std::function<void(void*, int)> handler_for_pool_exhausted_ = [this]([[maybe_unused]] void* for_sender_client_use, [[maybe_unused]] int objects_per_pool) {
        this->pool_exhausted_callback_count_++;
    };

    std::function<void(void*, void*)> handler_for_invalid_free_ = [this]([[maybe_unused]] void* for_receiver_client_use,
                                                                         [[maybe_unused]] void* object_to_deallocate) { this->invalid_free_callback_count_++; };

    std::function<void(void*)> handler_for_huge_pages_error_ = [this]([[maybe_unused]] void* for_client_use) { this->huge_pages_error_callback_count_++; };

    // Convenience factory.
    ExpandablePoolAllocator<TestObject> make_allocator(std::string name, int objects_per_pool, int initial_pools = 1, int threshold = 1) {
        return {std::move(name),
                objects_per_pool,
                initial_pools,
                threshold,
                handler_for_pool_exhausted_,
                handler_for_invalid_free_,
                handler_for_huge_pages_error_,
                UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages)};
    }
};

TEST_F(ExpandablePoolAllocatorTest, BasicAllocationAndDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    auto allocator = make_allocator("BasicTest", objects_per_pool, initial_pools, expansion_threshold);

    std::vector<TestObject*> allocated_objects;
    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        allocated_objects.push_back(obj);
    }

    EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool);

    for (TestObject* obj : allocated_objects) {
        allocator.deallocate(obj);
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), objects_per_pool);
}

TEST_F(ExpandablePoolAllocatorTest, PoolExpansionOnExhaustion) {
    const int objects_per_pool = 5;
    const int initial_pools = 1;
    const int expansion_threshold = 2;

    auto allocator = make_allocator("ExpansionTest", objects_per_pool, initial_pools, expansion_threshold);

    std::vector<TestObject*> allocated_objects;
    for (int i = 0; i < objects_per_pool + 1; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        allocated_objects.push_back(obj);
    }

    EXPECT_EQ(pool_exhausted_callback_count_.load(), 1);
    EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool + 1);

    for (TestObject* obj : allocated_objects) {
        allocator.deallocate(obj);
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), objects_per_pool + 1);
}

TEST_F(ExpandablePoolAllocatorTest, MaxChainLengthEnforcement) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    ExpandablePoolAllocator<int> allocator("MaxChainLengthEnforcement", objects_per_pool, initial_pools, expansion_threshold, handler_for_pool_exhausted_,
                                           handler_for_invalid_free_, handler_for_huge_pages_error_, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::vector<int*> pointers;
    pointers.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        int* pointer = allocator.allocate();
        ASSERT_NE(pointer, nullptr) << "allocator returned nullptr during initial allocation";
        pointers.push_back(pointer);
    }

    auto statistics_after_initial_allocations = allocator.get_behaviour_statistics();

    // double-free: is_constructed check should catch it
    for (int* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    auto statistics_after_deallocation = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_after_initial_allocations.expansion_events, statistics_after_deallocation.expansion_events)
        << "deallocation must not trigger pool expansion";

    for (int* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    auto statistics_after_over_free = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_after_deallocation.expansion_events, statistics_after_over_free.expansion_events)
        << "over-free attempts must not cause pool expansion";

    std::vector<int*> pointers_second_round;
    pointers_second_round.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        int* pointer = allocator.allocate();
        ASSERT_NE(pointer, nullptr) << "allocator failed after over-free attempts";
        pointers_second_round.push_back(pointer);
    }

    auto statistics_after_second_allocations = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_after_over_free.expansion_events, statistics_after_second_allocations.expansion_events)
        << "allocator should not expand when reusing the existing pool";

    for (int* pointer : pointers_second_round) {
        allocator.deallocate(pointer);
    }
}

TEST_F(ExpandablePoolAllocatorTest, ConcurrentAllocationAndDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 8;
    const int64_t total_threads = 4; // not an int due to use in multiplication result
    const int allocations_per_thread = 20;

    auto allocator = make_allocator("ConcurrentTest", objects_per_pool, initial_pools, expansion_threshold);

    std::vector<TestObject*> thread_safe_allocated_objects_ptr_list(total_threads * allocations_per_thread);
    std::atomic<int> thread_safe_counter(0);
    std::vector<std::thread> threads;
    threads.reserve(total_threads);
    for (int i = 0; i < total_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < allocations_per_thread; ++j) {
                TestObject* obj = allocator.allocate();
                if (obj != nullptr) {
                    obj->id_ = j + 1;
                    const int index = thread_safe_counter.fetch_add(1, std::memory_order_relaxed);
                    thread_safe_allocated_objects_ptr_list[index] = obj;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    const int total_expected_allocations = total_threads * allocations_per_thread;
    const int successful_allocations = thread_safe_counter.load();

    EXPECT_EQ(successful_allocations, total_expected_allocations);
    EXPECT_EQ(TestObject::s_constructor_count.load(), total_expected_allocations);

    // Note: We do not care that on some embedded Linux targets `std::random_device` is deterministic, this is just a test after all.
    std::shuffle(thread_safe_allocated_objects_ptr_list.begin(), thread_safe_allocated_objects_ptr_list.end(),
                 std::default_random_engine(std::random_device()()));

    for (auto* obj : thread_safe_allocated_objects_ptr_list) {
        if (obj != nullptr) {
            allocator.deallocate(obj);
        }
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), total_expected_allocations);
}

TEST_F(ExpandablePoolAllocatorTest, InvalidDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    auto allocator = make_allocator("InvalidDeallocationTest", objects_per_pool, initial_pools, expansion_threshold);

    TestObject invalid_object;
    allocator.deallocate(&invalid_object);

    EXPECT_EQ(invalid_free_callback_count_.load(), 1);
}

TEST_F(ExpandablePoolAllocatorTest, HugePagesBehavior) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    const ExpandablePoolAllocator<TestObject> allocator_hp("HugePagesTest", objects_per_pool, initial_pools, expansion_threshold, handler_for_pool_exhausted_,
                                                           handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                           UseHugePagesFlag(UseHugePagesFlag::DoUseHugePages));

    EXPECT_GE(huge_pages_error_callback_count_.load(), 0);

    huge_pages_error_callback_count_ = 0;
    const ExpandablePoolAllocator<TestObject> allocator_no_hp("NoHugePagesTest", objects_per_pool, initial_pools, expansion_threshold,
                                                              handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                              UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    EXPECT_EQ(huge_pages_error_callback_count_.load(), 0);
}

TEST_F(ExpandablePoolAllocatorTest, ThunderingHerdExpansionRace) {
    const int objects_per_pool = 1;
    const int initial_pools = 1;
    const int expansion_threshold = 100;
    const int num_threads = 80;

    ExpandablePoolAllocator<TestObject> allocator("RaceTest", objects_per_pool, initial_pools, expansion_threshold, handler_for_pool_exhausted_,
                                                  handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::vector<TestObject*> results(num_threads, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            while (!start_gate) {
                std::this_thread::yield();
            }
            results[i] = allocator.allocate();
        });
    }

    start_gate = true;
    for (auto& t : threads) {
        t.join();
    }

    int success_count = 0;
    for (auto* ptr : results) {
        if (ptr != nullptr) {
            success_count++;
            allocator.deallocate(ptr);
        }
    }

    EXPECT_EQ(success_count, num_threads);
    EXPECT_EQ(pool_exhausted_callback_count_.load(), num_threads - initial_pools);
}

TEST_F(ExpandablePoolAllocatorTest, ProducerConsumerStressTest) {
    const int objects_per_pool = 100;
    const int initial_pools = 1;
    const int expansion_threshold = 50;
    const int num_producers = 4;
    const int num_consumers = 4;
    const int items_per_producer = 1000;

    auto allocator = make_allocator("StressTest", objects_per_pool, initial_pools, expansion_threshold);

    std::vector<TestObject*> queue;
    std::mutex queue_mutex;
    std::atomic<bool> production_finished{false};
    std::atomic<int> total_consumed{0};

    std::vector<std::thread> producers;
    producers.reserve(num_producers);
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            for (int j = 0; j < items_per_producer; ++j) {
                TestObject* obj = nullptr;
                while ((obj = allocator.allocate()) == nullptr) {
                    std::this_thread::yield();
                }
                obj->id_ = j;
                const std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push_back(obj);
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&]() {
            bool should_exit = false;
            while (!should_exit) {
                TestObject* obj = nullptr;
                {
                    const std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!queue.empty()) {
                        obj = queue.back();
                        queue.pop_back();
                    } else if (production_finished.load(std::memory_order_acquire)) {
                        should_exit = true;
                    }
                }
                if (obj != nullptr) {
                    allocator.deallocate(obj);
                    total_consumed++;
                } else if (!should_exit) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }
    production_finished = true;
    for (auto& t : consumers) {
        t.join();
    }

    EXPECT_EQ(total_consumed.load(), num_producers * items_per_producer);
    EXPECT_EQ(TestObject::s_destructor_count.load(), total_consumed.load());
}

TEST_F(ExpandablePoolAllocatorTest, CacheLineContentionStress) {
    const int objects_per_pool = 1000;
    const int num_threads = 12;
    const int iterations = 5000;

    auto allocator = make_allocator("ContentionTest", objects_per_pool, 1, 1);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                TestObject* obj = allocator.allocate();
                if (obj != nullptr) {
                    obj->id_ = j;
                    allocator.deallocate(obj);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0);
}

TEST_F(ExpandablePoolAllocatorTest, PoolCorrectnessAndReuse) {
    const int objects_per_pool = 8;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    auto allocator = make_allocator("PoolCorrectness", objects_per_pool, initial_pools, expansion_threshold);

    std::vector<TestObject*> ptrs;
    ptrs.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        ptrs.push_back(obj);
    }

    std::vector<TestObject*> shuffled = ptrs;
    std::shuffle(shuffled.begin(), shuffled.end(), std::default_random_engine(std::random_device()()));

    for (auto* obj : shuffled) {
        allocator.deallocate(obj);
    }

    std::vector<TestObject*> ptrs2;
    ptrs2.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        ptrs2.push_back(obj);
    }

    // Sort both vectors before comparison
    std::sort(ptrs.begin(), ptrs.end());
    std::sort(ptrs2.begin(), ptrs2.end());
    EXPECT_EQ(ptrs, ptrs2);

    for (auto* obj : ptrs2) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, DestructorReleasesAllObjects) {
    const int objects_per_pool = 16;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    {
        auto allocator = make_allocator("AllocatorDestruction", objects_per_pool, initial_pools, expansion_threshold);

        for (int i = 0; i < objects_per_pool; ++i) {
            TestObject* obj = allocator.allocate();
            ASSERT_NE(obj, nullptr);
        }

        EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool);
        EXPECT_EQ(TestObject::s_destructor_count.load(), 0);
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), TestObject::s_constructor_count.load());
}

TEST_F(ExpandablePoolAllocatorTest, DeterministicThunderingHerdOrdering) {
    const int objects_per_pool = 4;
    const int thread_count = 8;

    ExpandablePoolAllocator<int> allocator("DeterministicThunderingHerdOrdering", objects_per_pool, 1, 1, handler_for_pool_exhausted_,
                                           handler_for_invalid_free_, handler_for_huge_pages_error_, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::vector<int*> results(thread_count, nullptr);
    std::atomic<bool> start_flag{false};
    std::atomic<int> ready_count{0};

    auto stats_before = allocator.get_behaviour_statistics();

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&, i]() {
            ready_count.fetch_add(1);
            while (!start_flag.load()) {
                std::this_thread::yield();
            }
            results[i] = allocator.allocate();
        });
    }

    while (ready_count.load() != thread_count) {
        std::this_thread::yield();
    }
    start_flag.store(true);
    for (auto& t : threads) {
        t.join();
    }

    auto stats_after = allocator.get_behaviour_statistics();

    // At least one expansion must have occurred (8 threads, 4-slot pool).
    EXPECT_GE(stats_after.expansion_events, stats_before.expansion_events + 1) << "at least one expansion must occur when demand exceeds pool capacity";

    // Every thread must have received a valid, unique pointer.
    for (int i = 0; i < thread_count; ++i) {
        ASSERT_NE(results[i], nullptr) << "thread " << i << " got nullptr";
    }
    for (int i = 0; i < thread_count; ++i) {
        for (int j = i + 1; j < thread_count; ++j) {
            EXPECT_NE(results[i], results[j]) << "threads " << i << " and " << j << " got duplicate pointers";
        }
    }

    for (int* p : results) {
        allocator.deallocate(p);
    }
}

TEST_F(ExpandablePoolAllocatorTest, NullptrDeallocateThrows) {
    auto allocator = make_allocator("NullptrTest", 10);
    EXPECT_THROW(allocator.deallocate(nullptr), PreconditionAssertion) << "deallocate(nullptr) must throw PreconditionAssertion";
    EXPECT_EQ(invalid_free_callback_count_.load(), 0) << "invalid_free callback must not fire for nullptr (exception fires first)";
}

TEST_F(ExpandablePoolAllocatorTest, LatencyStressTest) {
    const int num_threads = 80;
    const int objects_per_pool = 1000;
    const int iterations = 100;

    // 1. Setup: Start with 1 initial pool to observe the transition
    // from steady-state to chained-state.
    ExpandablePoolAllocator<TestObject> allocator("LatencyTest", objects_per_pool, 1, 10, handler_for_pool_exhausted_, handler_for_invalid_free_,
                                                  handler_for_huge_pages_error_, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    LatencyRecorder alloc_recorder;
    LatencyRecorder dealloc_recorder;

    // 2. PRIMING / PRE-FILL: Saturated the first pool to 90% (900 objects).
    // This establishes the 'Hot Buckets' (20-40ns) in a single-threaded
    // environment to prevent map-insertion race conditions/core dumps.
    std::vector<TestObject*> sentinel_objects;
    sentinel_objects.reserve(900);

    for (int i = 0; i < 900; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        TestObject* obj = allocator.allocate();
        auto end = std::chrono::high_resolution_clock::now();

        if (obj != nullptr) {
            alloc_recorder.record(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            sentinel_objects.push_back(obj);
        }
    }

    // 3. MULTI-THREADED STRESS: 80 threads fight for the remaining 100 slots
    // in Pool 1, then trickle into Pool 2 and 3.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                // Measure Allocation
                auto start = std::chrono::high_resolution_clock::now();
                TestObject* obj = allocator.allocate();
                auto end = std::chrono::high_resolution_clock::now();

                if (obj != nullptr) {
                    alloc_recorder.record(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

                    // Measure Deallocation
                    // Since deallocate() is O(N) searching from head_pool_,
                    // this will reveal the 'Search Tax' for chained pools.
                    start = std::chrono::high_resolution_clock::now();
                    allocator.deallocate(obj);
                    end = std::chrono::high_resolution_clock::now();
                    dealloc_recorder.record(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 4. CLEANUP: Return the pre-fill sentinels to the pool
    for (auto* obj : sentinel_objects) {
        allocator.deallocate(obj);
    }

    // 5. OUTPUT: Space-delimited for Python/Gnuplot/Unix scanning.
    alloc_recorder.dump_space_delimited("ALLOCATION");
    dealloc_recorder.dump_space_delimited("DEALLOCATION");

    // Optional human-readable summary for console inspection
    alloc_recorder.dump_results("Allocation Summary");
}

TEST_F(ExpandablePoolAllocatorTest, AbaStressTest) {
#ifdef USING_VALGRIND
    const int iterations = 200;
    const int num_threads = 4;
#else
    const int iterations = 100'000;
    const int num_threads = 8;
#endif

    // Pool must be at least as large as the number of threads.
    // Each thread holds at most one slot at a time, so with pool_slots >= num_threads
    // the pool can never be fully exhausted and expansion will never fire.
    const int pool_slots = num_threads;
    const int initial_pools = 1;
    const int expansion_threshold_hint = 1;
    ExpandablePoolAllocator<TestObject> allocator("AbaStress", pool_slots, initial_pools, expansion_threshold_hint, handler_for_pool_exhausted_,
                                                  handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> stop{false};

    // Collect every address returned by allocate() during the stress phase.
    // We do this concurrently using a mutex-protected set so that valid_addresses
    // contains every slot that was ever legitimately allocated, including any
    // slots created by pool expansion. This avoids having to predict how many
    // slots will exist after the stress phase.
    std::set<TestObject*> valid_addresses;
    std::mutex valid_addresses_mutex;

    // Each thread repeatedly allocates and immediately deallocates.
    // We deliberately do NOT hold any lock — this is the ABA window.
    // If the Treiber stack's ABA prevention is broken, two threads could
    // receive the same pointer simultaneously, which would show up as a
    // duplicate in valid_addresses or a corrupted slot count in the drain phase.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int thread_index = 0; thread_index < num_threads; ++thread_index) {
        threads.emplace_back([&]() {
            for (int iteration = 0; iteration < iterations && !stop.load(std::memory_order_relaxed); ++iteration) {
                TestObject* allocated_object = allocator.allocate();
                if (allocated_object == nullptr) {
                    continue;
                }
                // Record every address we legitimately receive.
                {
                    const std::lock_guard<std::mutex> lock(valid_addresses_mutex);
                    valid_addresses.insert(allocated_object);
                }
                allocator.deallocate(allocated_object);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // After all threads finish, drain the pool and verify structural integrity.
    // We use get_pool_statistics() to find the true total capacity across all
    // pools including any created by expansion during the stress phase.
    // We allocate exactly that many objects — no more, no less — so we never
    // trigger further expansion.
    auto pool_stats_before_drain = allocator.get_pool_statistics();
    const int total_capacity = pool_stats_before_drain.number_of_pools_ * pool_stats_before_drain.number_of_objects_per_pool_;

    std::vector<TestObject*> drained_objects;
    drained_objects.reserve(total_capacity);
    for (int slot_index = 0; slot_index < total_capacity; ++slot_index) {
        TestObject* allocated_object = allocator.allocate();
        if (allocated_object == nullptr) {
            ADD_FAILURE() << "free-list corruption: pool returned nullptr at slot " << slot_index << " of expected " << total_capacity;
            break;
        }
        drained_objects.push_back(allocated_object);
    }

    // Every drained address must have been seen during the stress phase.
    // An address that was never returned by allocate() during stress would
    // indicate the free list has grown a corrupt extra entry.
    std::unordered_set<TestObject*> seen_during_drain;
    for (auto* drained_object : drained_objects) {
        EXPECT_TRUE(seen_during_drain.insert(drained_object).second) << "free-list corruption: duplicate pointer " << drained_object << " after ABA stress";
        EXPECT_NE(valid_addresses.find(drained_object), valid_addresses.end()) << "free-list corruption: unknown pointer " << drained_object;
    }

    // All slots should be recoverable — the number drained must equal the total
    // capacity across all pools, including any created by expansion during stress.
    // A smaller count means slots were lost; a larger count means slots were
    // duplicated in the free list — both indicate Treiber stack corruption.
    EXPECT_EQ(static_cast<int>(drained_objects.size()), total_capacity)
        << "free-list corruption: expected " << total_capacity << " slots drained, got " << drained_objects.size();

    for (auto* drained_object : drained_objects) {
        allocator.deallocate(drained_object);
    }
}

/**
 * @brief Verifies that the double-free guard in deallocate() fires the
 * invalid_free callback and does not corrupt the free list.
 *
 * Deallocates the same pointer twice. The second call must trigger the
 * invalid_free callback exactly once via the atomic CAS on is_constructed.
 * After the double-free attempt the pool must remain fully usable — all
 * slots must still be allocatable and unique.
 */
TEST_F(ExpandablePoolAllocatorTest, DoubleFreeDetection) {
    auto allocator = make_allocator("DoubleFreeTest", 4);

    TestObject* obj = allocator.allocate();
    ASSERT_NE(obj, nullptr);

    allocator.deallocate(obj); // valid free
    EXPECT_EQ(invalid_free_callback_count_.load(), 0);

    allocator.deallocate(obj); // double free — is_constructed should catch it
    EXPECT_EQ(invalid_free_callback_count_.load(), 1);

    // Pool must remain fully usable after a double-free attempt.
    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0);

    // All 4 slots must still be allocatable.
    std::vector<TestObject*> ptrs;
    for (int i = 0; i < 4; ++i) {
        auto* p = allocator.allocate();
        ASSERT_NE(p, nullptr) << "pool corrupt after double-free: slot " << i;
        ptrs.push_back(p);
    }
    expect_unique_non_null(ptrs, "DoubleFreeDetection post-check");

    for (auto* p : ptrs) {
        allocator.deallocate(p);
    }
}

/**
 * ConcurrentDoubleFreeStress.
 *
 * Many threads race to free the same set of objects. Only the first
 * deallocate() on each object is valid; subsequent ones must be caught by
 * the is_constructed guard and must not corrupt the free list.
 *
 * Final invariant: zero allocated objects remain after all threads exit.
 */
TEST_F(ExpandablePoolAllocatorTest, ConcurrentDoubleFreeStress) {
#ifdef USING_VALGRIND
    const int num_threads = 4;
    const int pool_size = 20;
#else
    const int num_threads = 16;
    const int pool_size = 64;
#endif

    ExpandablePoolAllocator<TestObject> allocator("ConcurrentDoubleFree", pool_size, 1, 1, handler_for_pool_exhausted_, handler_for_invalid_free_,
                                                  handler_for_huge_pages_error_, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    // Allocate all slots up front.
    std::vector<TestObject*> ptrs;
    ptrs.reserve(pool_size);
    for (int i = 0; i < pool_size; ++i) {
        auto* p = allocator.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }

    std::atomic<bool> gate{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    // Every thread tries to free every pointer.  Only the first deallocate()
    // per pointer is valid; the rest should be silently handled by the guard.
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (auto* p : ptrs) {
                allocator.deallocate(p);
            }
        });
    }

    gate.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0) << "pool has leaked objects after concurrent double-free stress";

    // Pool must still be fully usable.
    std::vector<TestObject*> recovered;
    for (int i = 0; i < pool_size; ++i) {
        auto* p = allocator.allocate();
        ASSERT_NE(p, nullptr) << "pool corrupt: slot " << i << " missing after stress";
        recovered.push_back(p);
    }
    expect_unique_non_null(recovered, "ConcurrentDoubleFreeStress");
    for (auto* p : recovered) {
        allocator.deallocate(p);
    }
}

/**
 * FrequentExpansionAndShrinkPattern.
 *
 * Exercises the scenario where current_pool_ptr_ points to a full later
 * pool while earlier pools have free slots. Verifies that the slow path
 * correctly finds the earlier pool and updates current_pool_ptr_, and that
 * no objects are lost across multiple expand/fill/empty cycles.
 */
TEST_F(ExpandablePoolAllocatorTest, FrequentExpansionAndShrinkPattern) {
    const int pool_size = 4;
    const int cycles = 8;

    auto allocator = make_allocator("ExpansionShrink", pool_size);

    std::vector<TestObject*> held;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        // Fill until we need an expansion.
        for (int i = 0; i < pool_size; ++i) {
            auto* p = allocator.allocate();
            ASSERT_NE(p, nullptr) << "cycle " << cycle << " slot " << i;
            held.push_back(p);
        }
        // Free all (exercises slow path on next iteration when current_pool_ptr_
        // may point to a now-full later pool).
        for (auto* p : held) {
            allocator.deallocate(p);
        }
        held.clear();
    }

    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0) << "memory leak after expansion/shrink cycles";
}

/**
 * StatisticsConsistency.
 *
 * Verifies the invariants documented in BehaviouralStatisticsStressTest
 * under lighter load so failures are reproducible on any machine.
 */
TEST_F(ExpandablePoolAllocatorTest, StatisticsConsistency) {
    const int pool_size = 32;
    const int num_threads = 8;
    const int iterations = 500;

    auto allocator = make_allocator("StatsConsistency", pool_size);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                TestObject* obj = allocator.allocate();
                std::this_thread::yield();
                if (obj != nullptr) {
                    allocator.deallocate(obj);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto stats = allocator.get_behaviour_statistics();

    EXPECT_EQ(stats.fast_path_allocations + stats.slow_path_allocations, stats.total_allocations) << "fast + slow must equal total";

    ASSERT_GE(stats.per_pool_allocation_counts.counts.size(), 1U);

    uint64_t sum = 0;
    for (auto c : stats.per_pool_allocation_counts.counts) {
        sum += c;
    }
    EXPECT_LE(sum, stats.total_allocations) << "per-pool sum must not exceed total allocations";
    EXPECT_GT(sum, 0U);

    auto pool_stats = allocator.get_pool_statistics();
    EXPECT_EQ(pool_stats.number_of_allocated_objects_, 0) << "no objects should be outstanding after all threads join";
}

TEST_F(ExpandablePoolAllocatorTest, BehaviouralStatisticsStressTest) {
#ifdef USING_VALGRIND
    const int num_threads = 10; // 120 is too many for instrumented serial execution
    const int iterations = 100; // Reduced to allow completion in seconds, not hours
#else
    const int num_threads = 120; // Increased to raise contention
    const int iterations = 2000; // More cycles → more slow-path
#endif
    const int objects_per_pool = 64; // Keep pool large enough to avoid expansion
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    ExpandablePoolAllocator<TestObject> allocator("BehaviourStatsTest", objects_per_pool, initial_pools, expansion_threshold, handler_for_pool_exhausted_,
                                                  handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&allocator]() {

        // Deterministic per-thread jitter (1–10 microseconds)
#ifndef USING_VALGRIND
            const auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
            const int jitter_us = 1 + static_cast<int>(tid_hash % 10);
#endif
            for (int j = 0; j < iterations; ++j) {
                TestObject* obj = allocator.allocate();

                // Increase contention without forcing expansion
#ifdef USING_VALGRIND
                std::this_thread::yield();
#else
                std::this_thread::sleep_for(std::chrono::microseconds(jitter_us));
#endif
                if (obj != nullptr) {
                    allocator.deallocate(obj);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto stats = allocator.get_behaviour_statistics();

    // --- Correctness invariants (V2-accurate) ---

    ASSERT_EQ(stats.fast_path_allocations + stats.slow_path_allocations, stats.total_allocations) << "fast + slow must equal total allocations";

    ASSERT_GE(stats.per_pool_allocation_counts.counts.size(), 1U) << "allocator must have at least one pool";

    uint64_t sum_per_pool = 0;
    for (const uint64_t count : stats.per_pool_allocation_counts.counts) {
        sum_per_pool += count;
    }

    ASSERT_LE(sum_per_pool, stats.total_allocations) << "sum of per-pool counts must never exceed total allocations";

    ASSERT_GT(sum_per_pool, 0U) << "at least one pool must have been used";

    // --- Diagnostic output block (machine-parseable) ---

    std::cout << "# DATASET: BEHAVIOUR-STATS\n";
    std::cout << "total_allocations " << stats.total_allocations << "\n";
    std::cout << "fast_path_allocations " << stats.fast_path_allocations << "\n";
    std::cout << "slow_path_allocations " << stats.slow_path_allocations << "\n";
    std::cout << "expansion_events " << stats.expansion_events << "\n";
    std::cout << "failed_allocations " << stats.failed_allocations << "\n";
    std::cout << "pool_count " << stats.per_pool_allocation_counts.counts.size() << "\n";

    std::cout << "per_pool_counts";
    for (const uint64_t count : stats.per_pool_allocation_counts.counts) {
        std::cout << " " << count;
    }
    std::cout << "\n";

    std::cout << "# END-DATASET\n";
}

/**
 * ConcurrentInvalidFreeCallbackRace
 *
 * Demonstrates that handler_for_invalid_free_ can be invoked concurrently
 * by multiple threads before the callback_mutex_ fix is applied.
 *
 * The callback uses an atomic in-progress counter. If two threads are ever
 * inside the callback simultaneously, the peak concurrency counter will
 * exceed 1, proving the race exists.
 *
 * After the callback_mutex_ fix is applied, peak concurrency must always
 * be exactly 1 — only one thread can be inside the callback at a time.
 *
 * The test exercises both invalid-free code paths:
 *   (a) pointer does not belong to any pool
 *   (b) double-free of a previously freed pointer (is_constructed == 0)
 */
TEST_F(ExpandablePoolAllocatorTest, ConcurrentInvalidFreeCallbackRace) {
#ifdef USING_VALGRIND
    const int num_threads = 4;
    const int iterations_per_thread = 200;
#else
    const int num_threads = 16;
    const int iterations_per_thread = 10'000;
#endif

    std::atomic<int> callback_in_progress{0};
    std::atomic<int> peak_concurrency{0};
    std::atomic<int> total_callback_invocations{0};

    // Replace the fixture callback with one that measures concurrency.
    std::function<void(void*, void*)> concurrency_measuring_callback = [&](void*, void*) {
        int concurrent = callback_in_progress.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Record the highest concurrency we have ever seen.
        int observed_peak = peak_concurrency.load(std::memory_order_relaxed);
        while (concurrent > observed_peak) {
            if (peak_concurrency.compare_exchange_weak(observed_peak, concurrent, std::memory_order_relaxed)) {
                break;
            }
        }

        // Simulate a lightweight callback doing some work.
        std::this_thread::yield();

        total_callback_invocations.fetch_add(1, std::memory_order_relaxed);
        callback_in_progress.fetch_sub(1, std::memory_order_acq_rel);
    };

    const int pool_size = 8;
    const int initial_pools = 1;
    const int expansion_threshold_hint = 1;
    ExpandablePoolAllocator<TestObject> allocator("CallbackRaceTest", pool_size, initial_pools, expansion_threshold_hint, handler_for_pool_exhausted_,
                                                  concurrency_measuring_callback, // our measuring callback
                                                  handler_for_huge_pages_error_, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    // Allocate one object that we will double-free from many threads,
    // and keep a stack object whose address is not in any pool.
    TestObject* valid_object = allocator.allocate();
    ASSERT_NE(valid_object, nullptr);
    allocator.deallocate(valid_object); // now is_constructed == 0, double-free bait

    TestObject stack_object; // not in the pool at all

    std::atomic<bool> start_gate{false};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int thread_index = 0; thread_index < num_threads; ++thread_index) {
        threads.emplace_back([&]() {
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int iteration = 0; iteration < iterations_per_thread; ++iteration) {
                // Alternate between the two invalid-free paths so both
                // code paths in deallocate() are hammered concurrently.
                if (iteration % 2 == 0) {
                    // Path (a): pointer not in any pool.
                    allocator.deallocate(&stack_object);
                } else {
                    // Path (b): double-free — is_constructed is 0 after the
                    // initial valid deallocate above.
                    allocator.deallocate(valid_object);
                }
            }
        });
    }

    start_gate.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "ConcurrentInvalidFreeCallbackRace:\n"
              << "  total callback invocations : " << total_callback_invocations.load() << "\n"
              << "  peak concurrent callbacks  : " << peak_concurrency.load() << "\n";

    // Before the fix: peak_concurrency will likely be > 1, proving the race.
    // After the fix:  peak_concurrency must be exactly 1.
    EXPECT_EQ(peak_concurrency.load(), 1) << "handler_for_invalid_free_ was invoked concurrently by " << peak_concurrency.load() << " threads simultaneously. "
                                          << "This demonstrates the race that callback_mutex_ is intended to prevent.";

    EXPECT_GT(total_callback_invocations.load(), 0) << "callback was never invoked — test did not exercise the code path";
}

TEST_F(ExpandablePoolAllocatorTest, MisalignedPointerRejectedByContains) {
    // We need direct access to FixedSizeMemoryPool to test contains().
    // Create one directly rather than through ExpandablePoolAllocator.
    pubsub_itc_fw::FixedSizeMemoryPool<TestObject> pool(4, UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages), [](void*, std::size_t) {});

    TestObject* valid_object = pool.allocate();
    ASSERT_NE(valid_object, nullptr);

    // A pointer one byte past a valid object — within the pool's memory
    // region but not aligned to a slot boundary.
    auto* misaligned_ptr = reinterpret_cast<TestObject*>(reinterpret_cast<std::byte*>(valid_object) + 1); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    // Before fix: contains() returns true — bounds check only.
    // After fix:  contains() returns false — alignment check added.
    EXPECT_FALSE(pool.contains(misaligned_ptr)) << "contains() must reject a pointer that is within the pool's "
                                                << "memory region but not aligned to a slot boundary";

    // A valid pointer must still be accepted.
    EXPECT_TRUE(pool.contains(valid_object)) << "contains() must accept a valid slot pointer";

    pool.deallocate(valid_object);
}

TEST_F(ExpandablePoolAllocatorTest, AbaStressWithMidDrain) {
#ifdef USING_VALGRIND
    const int iterations = 200;
    const int num_threads = 4;
    const int num_mid_drains = 4;
#else
    const int iterations = 100'000;
    const int num_threads = 8;
    const int num_mid_drains = 16;
#endif

    const int pool_slots = num_threads;
    const int initial_pools = 1;
    const int expansion_threshold_hint = 1;

    ExpandablePoolAllocator<TestObject> allocator("AbaStressWithMidDrain", pool_slots, initial_pools, expansion_threshold_hint, handler_for_pool_exhausted_,
                                                  handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    // All addresses ever legitimately returned by allocate() during the stress phase.
    std::set<TestObject*> valid_addresses;
    std::mutex valid_addresses_mutex;

    // Worker threads: allocate/deallocate hammer.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int thread_index = 0; thread_index < num_threads; ++thread_index) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int iteration = 0; iteration < iterations; ++iteration) {
                TestObject* allocated_object = allocator.allocate();
                if (allocated_object == nullptr) {
                    continue;
                }

                {
                    const std::lock_guard<std::mutex> lock(valid_addresses_mutex);
                    valid_addresses.insert(allocated_object);
                }

                allocator.deallocate(allocated_object);

                if ((static_cast<unsigned>(iteration) & 0xFFU) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Start all workers together.
    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    // Mid‑run partial drains: steal a subset of slots from the allocator,
    // verify structural invariants on that subset, then return them.
    for (int drain_round = 0; drain_round < num_mid_drains; ++drain_round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Snapshot current capacity across all pools.
        auto stats = allocator.get_pool_statistics();
        const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

        const int max_drain = std::max(1, total_capacity / 2);
        std::vector<TestObject*> drained;
        drained.reserve(max_drain);

        for (int i = 0; i < max_drain; ++i) {
            TestObject* obj = allocator.allocate();
            if (obj == nullptr) {
                // Some slots are in use by workers; stop this mid‑drain.
                break;
            }
            drained.push_back(obj);
        }

        if (!drained.empty()) {
            // Structural check: no duplicates in the mid‑drain slice.
            std::unordered_set<TestObject*> seen;
            for (auto* obj : drained) {
                EXPECT_TRUE(seen.insert(obj).second) << "allocator free-list corruption: duplicate pointer " << obj << " in mid‑drain round " << drain_round;
                // NOTE: we deliberately do NOT require membership in valid_addresses
                // here, because mid‑drain may be the first user of a slot from a
                // newly expanded pool under coverage/timing variations.
            }
        }

        for (auto* obj : drained) {
            allocator.deallocate(obj);
        }
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // After all threads finish, drain the pool and verify structural integrity.
    auto pool_stats_before_drain = allocator.get_pool_statistics();
    const int total_capacity = pool_stats_before_drain.number_of_pools_ * pool_stats_before_drain.number_of_objects_per_pool_;

    std::vector<TestObject*> drained_objects;
    drained_objects.reserve(total_capacity);
    for (int slot_index = 0; slot_index < total_capacity; ++slot_index) {
        TestObject* allocated_object = allocator.allocate();
        if (allocated_object == nullptr) {
            ADD_FAILURE() << "free-list corruption: allocator returned nullptr at slot " << slot_index << " of expected " << total_capacity;
            break;
        }
        drained_objects.push_back(allocated_object);
    }

    // Every drained address must be unique. We no longer require that every
    // drained address appeared in valid_addresses, because under coverage/
    // timing variations a pool may expand late and some slots may only ever
    // be observed during this final drain.
    std::unordered_set<TestObject*> seen_during_drain;
    for (auto* drained_object : drained_objects) {
        EXPECT_TRUE(seen_during_drain.insert(drained_object).second)
            << "free-list corruption: duplicate pointer " << drained_object << " after ABA mid‑drain stress";
    }

    // All slots should still be recoverable — the number drained must equal the total capacity across all pools.
    EXPECT_EQ(static_cast<int>(drained_objects.size()), total_capacity)
        << "free-list corruption: expected " << total_capacity << " slots drained, got " << drained_objects.size();

    for (auto* drained_object : drained_objects) {
        allocator.deallocate(drained_object);
    }
}

TEST_F(ExpandablePoolAllocatorTest, CrossPoolAbaInterleaving) {
#ifdef USING_VALGRIND
    const int iterations = 200;
    const int num_threads = 4;
    const int num_mid_drains = 4;
#else
    const int iterations = 100'000;
    const int num_threads = 8;
    const int num_mid_drains = 12;
#endif

    // Small pool to force expansion quickly.
    const int objects_per_pool = 4;
    const int initial_pools = 1;
    const int threshold_hint = 1;

    ExpandablePoolAllocator<TestObject> allocator("CrossPoolAbaInterleaving", objects_per_pool, initial_pools, threshold_hint, handler_for_pool_exhausted_,
                                                  handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    // Worker threads: hammer allocate/deallocate, triggering expansions.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations; ++i) {
                TestObject* obj = allocator.allocate();
                if (obj == nullptr) {
                    continue;
                }

                allocator.deallocate(obj);

                if ((static_cast<unsigned>(i) & 0xFFU) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    // Mid‑run partial drains across all pools.
    for (int round = 0; round < num_mid_drains; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto stats = allocator.get_pool_statistics();
        const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

        const int max_drain = std::max(1, total_capacity / 2);
        std::vector<TestObject*> drained;
        drained.reserve(max_drain);

        for (int i = 0; i < max_drain; ++i) {
            TestObject* obj = allocator.allocate();
            if (obj == nullptr) {
                break;
            }
            drained.push_back(obj);
        }

        // Structural checks on drained slice: no duplicates.
        std::unordered_set<TestObject*> seen;
        for (auto* obj : drained) {
            EXPECT_TRUE(seen.insert(obj).second)
                << "allocator free-list corruption: duplicate pointer "
                << obj << " in mid-drain round " << round;
        }

        for (auto* obj : drained) {
            allocator.deallocate(obj);
        }
    }

    for (auto& th : threads) {
        th.join();
    }

    // Final drain: snapshot capacity once, then drain exactly that many slots
    // without triggering further expansion.
    auto stats = allocator.get_pool_statistics();
    const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

    std::vector<TestObject*> drained;
    drained.reserve(total_capacity);

    for (int i = 0; i < total_capacity; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr)
            << "allocator free-list corruption: nullptr during final drain at index "
            << i << " of " << total_capacity;
        drained.push_back(obj);
    }

    // Do NOT call allocator.allocate() again here: that would legitimately
    // trigger expansion and invalidate the notion of "total_capacity".

    // All drained pointers must be unique. This is the meaningful ABA
    // corruption detector: a duplicate pointer in the final drain is the
    // true signature of free-list corruption.
    //
    // Note: we do not check that every drained pointer was seen during the
    // stress phase. The pool may expand late -- during mid-drain rounds after
    // worker threads have wound down -- so some slots may never have been
    // allocated by any caller before the final drain. Wild pointers are already
    // guarded against by the canary and contains() checks in
    // ExpandablePoolAllocator::deallocate().
    std::unordered_set<TestObject*> final_seen;
    for (auto* obj : drained) {
        EXPECT_TRUE(final_seen.insert(obj).second)
            << "allocator free-list corruption: duplicate pointer in final drain";
    }

    for (auto* obj : drained) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, ConcurrentInvalidFreeDuringExpansion) {
#ifdef USING_VALGRIND
    const int iterations = 200;
    const int num_threads = 4;
#else
    const int iterations = 50'000;
    const int num_threads = 8;
#endif

    const int objects_per_pool = 4; // small to force expansion
    const int initial_pools = 1;
    const int threshold_hint = 1;

    std::atomic<int> invalid_free_count{0};

    // Wrap the existing invalid-free handler so we can count invocations.
    std::function<void(void*, void*)> counting_invalid_free_handler = // NOLINT(misc-const-correctness)
        [&](void* allocator_ptr, void* ptr) {
            invalid_free_count.fetch_add(1, std::memory_order_relaxed);
            if (handler_for_invalid_free_) {
                handler_for_invalid_free_(allocator_ptr, ptr);
            }
        };

    ExpandablePoolAllocator<TestObject> allocator("ConcurrentInvalidFreeDuringExpansion", objects_per_pool, initial_pools, threshold_hint,
                                                  handler_for_pool_exhausted_, counting_invalid_free_handler, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    // Worker threads: hammer allocate/deallocate, triggering expansions and occasional invalid frees.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations; ++i) {
                TestObject* obj = allocator.allocate();
                if (obj != nullptr) {
                    allocator.deallocate(obj);
                }

                // Occasionally attempt an invalid free with a stack object.
                if ((i % 1024) == 0) {
                    TestObject bogus;
                    allocator.deallocate(&bogus); // must trigger invalid-free
                }

                if ((static_cast<unsigned>(i) & 0xFFU) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    // After stress, allocator must still be structurally sound.
    auto stats = allocator.get_pool_statistics();
    const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

    std::vector<TestObject*> drained;
    drained.reserve(total_capacity);

    for (int i = 0; i < total_capacity; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr) << "allocator corruption after invalid-free stress";
        drained.push_back(obj);
    }

    // Must have detected at least some invalid frees.
    EXPECT_GT(invalid_free_count.load(std::memory_order_relaxed), 0) << "invalid-free handler was never invoked";

    // All drained pointers must be unique.
    std::unordered_set<TestObject*> seen;
    for (auto* obj : drained) {
        EXPECT_TRUE(seen.insert(obj).second) << "duplicate pointer detected after invalid-free stress";
    }

    for (auto* obj : drained) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, StalePointerInvalidFree) {
    const int objects_per_pool = 4;
    const int initial_pools = 1;
    const int threshold_hint = 1;

    std::atomic<int> invalid_free_count{0};

    // Wrap the existing invalid-free handler so we can count invocations.
    std::function<void(void*, void*)> counting_invalid_free_handler = // NOLINT(misc-const-correctness)
        [&](void* allocator_ptr, void* ptr) {
            invalid_free_count.fetch_add(1, std::memory_order_relaxed);
            if (handler_for_invalid_free_) {
                handler_for_invalid_free_(allocator_ptr, ptr);
            }
        };

    ExpandablePoolAllocator<TestObject> allocator("StalePointerInvalidFree", objects_per_pool, initial_pools, threshold_hint, handler_for_pool_exhausted_,
                                                  counting_invalid_free_handler, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    // Step 1: allocate and free once.
    TestObject* first = allocator.allocate();
    ASSERT_NE(first, nullptr);
    allocator.deallocate(first);

    // Step 2: reallocate; with LIFO free-list this should be the same slot.
    TestObject* second = allocator.allocate();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first, second) << "expected immediate reuse of freed slot";

    // Step 3: stale-pointer free: use the old pointer again.
    allocator.deallocate(first); // stale / double free, must be reported

    // Step 4: now free the "current" owner pointer.
    allocator.deallocate(second);

    // We must have seen exactly one invalid-free.
    EXPECT_EQ(invalid_free_count.load(std::memory_order_relaxed), 1) << "stale-pointer invalid free was not detected exactly once";

    // Final structural sanity: allocator still consistent.
    auto stats = allocator.get_pool_statistics();
    const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

    std::vector<TestObject*> drained;
    drained.reserve(total_capacity);

    for (int i = 0; i < total_capacity; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr) << "allocator corruption after stale-pointer invalid-free test";
        drained.push_back(obj);
    }

    std::unordered_set<TestObject*> seen;
    for (auto* obj : drained) {
        EXPECT_TRUE(seen.insert(obj).second) << "duplicate pointer detected after stale-pointer invalid-free test";
    }

    for (auto* obj : drained) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, InvalidFreeDuringMidDrain) {
#ifdef USING_VALGRIND
    const int iterations = 200;
    const int num_threads = 4;
    const int num_mid_drains = 4;
#else
    const int iterations = 80'000;
    const int num_threads = 8;
    const int num_mid_drains = 12;
#endif

    const int objects_per_pool = 4; // small to force expansion
    const int initial_pools = 1;
    const int threshold_hint = 1;

    std::atomic<int> invalid_free_count{0};

    // Wrap the existing invalid-free handler so we can count invocations.
    std::function<void(void*, void*)> counting_invalid_free_handler = // NOLINT(misc-const-correctness)
        [&](void* allocator_ptr, void* ptr) {
            invalid_free_count.fetch_add(1, std::memory_order_relaxed);
            if (handler_for_invalid_free_) {
                handler_for_invalid_free_(allocator_ptr, ptr);
            }
        };

    ExpandablePoolAllocator<TestObject> allocator("InvalidFreeDuringMidDrain", objects_per_pool, initial_pools, threshold_hint, handler_for_pool_exhausted_,
                                                  counting_invalid_free_handler, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    // Worker threads: hammer allocate/deallocate and occasionally perform invalid frees.
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations; ++i) {
                TestObject* obj = allocator.allocate();
                if (obj != nullptr) {
                    allocator.deallocate(obj);
                }

                // Occasionally attempt an invalid free with a stack object.
                if ((i % 2048) == 0) {
                    TestObject bogus;
                    allocator.deallocate(&bogus); // must trigger invalid-free
                }

                if ((static_cast<unsigned>(i) & 0xFFU) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    // Mid‑run partial drains: steal a subset of slots while workers are active.
    for (int round = 0; round < num_mid_drains; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto stats = allocator.get_pool_statistics();
        const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

        const int max_drain = std::max(1, total_capacity / 3);
        std::vector<TestObject*> drained;
        drained.reserve(max_drain);

        for (int i = 0; i < max_drain; ++i) {
            TestObject* obj = allocator.allocate();
            if (obj == nullptr) {
                break;
            }
            drained.push_back(obj);
        }

        // Structural check: no duplicates in the mid‑drain slice.
        std::unordered_set<TestObject*> seen;
        for (auto* obj : drained) {
            EXPECT_TRUE(seen.insert(obj).second) << "allocator free-list corruption: duplicate pointer " << obj << " in mid‑drain round " << round;
        }

        // Return drained objects.
        for (auto* obj : drained) {
            allocator.deallocate(obj);
        }
    }

    for (auto& th : threads) {
        th.join();
    }

    // After all stress, allocator must still be structurally sound.
    auto stats = allocator.get_pool_statistics();
    const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

    std::vector<TestObject*> drained;
    drained.reserve(total_capacity);

    for (int i = 0; i < total_capacity; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr) << "allocator corruption after invalid-free mid-drain stress";
        drained.push_back(obj);
    }

    // Must have detected at least some invalid frees.
    EXPECT_GT(invalid_free_count.load(std::memory_order_relaxed), 0) << "invalid-free handler was never invoked during mid-drain stress";

    // All drained pointers must be unique.
    std::unordered_set<TestObject*> seen;
    for (auto* obj : drained) {
        EXPECT_TRUE(seen.insert(obj).second) << "duplicate pointer detected after invalid-free mid-drain stress";
    }

    for (auto* obj : drained) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, CrossAllocatorInvalidFree) {
    const int objects_per_pool = 4;
    const int initial_pools = 1;
    const int threshold_hint = 1;

    std::atomic<int> invalid_free_count{0};

    // Wrap the existing invalid-free handler so we can count invocations for allocator B.
    std::function<void(void*, void*)> counting_invalid_free_handler = // NOLINT(misc-const-correctness)
        [&](void* allocator_ptr, void* ptr) {
            invalid_free_count.fetch_add(1, std::memory_order_relaxed);
            if (handler_for_invalid_free_) {
                handler_for_invalid_free_(allocator_ptr, ptr);
            }
        };

    // Allocator A: normal handlers.
    ExpandablePoolAllocator<TestObject> allocator_a("CrossAllocatorInvalidFree_A", objects_per_pool, initial_pools, threshold_hint, handler_for_pool_exhausted_,
                                                    handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                    UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    // Allocator B: counts invalid frees.
    ExpandablePoolAllocator<TestObject> allocator_b("CrossAllocatorInvalidFree_B", objects_per_pool, initial_pools, threshold_hint, handler_for_pool_exhausted_,
                                                    counting_invalid_free_handler, handler_for_huge_pages_error_,
                                                    UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    // Allocate from allocator A.
    TestObject* from_a = allocator_a.allocate();
    ASSERT_NE(from_a, nullptr);

    // Illegally free into allocator B: must be detected as invalid.
    allocator_b.deallocate(from_a);

    EXPECT_GT(invalid_free_count.load(std::memory_order_relaxed), 0) << "cross-allocator invalid free was not detected";

    // Structural sanity: allocator A still consistent.
    {
        auto stats_a = allocator_a.get_pool_statistics();
        const int total_capacity_a = stats_a.number_of_pools_ * stats_a.number_of_objects_per_pool_;

        std::vector<TestObject*> drained_a;
        drained_a.reserve(total_capacity_a);

        for (int i = 0; i < total_capacity_a; ++i) {
            TestObject* obj = allocator_a.allocate();
            ASSERT_NE(obj, nullptr) << "allocator A corruption after cross-allocator invalid-free test";
            drained_a.push_back(obj);
        }

        std::unordered_set<TestObject*> seen_a;
        for (auto* obj : drained_a) {
            EXPECT_TRUE(seen_a.insert(obj).second) << "allocator A duplicate pointer after cross-allocator invalid-free test";
        }

        for (auto* obj : drained_a) {
            allocator_a.deallocate(obj);
        }
    }

    // Structural sanity: allocator B still consistent.
    {
        auto stats_b = allocator_b.get_pool_statistics();
        const int total_capacity_b = stats_b.number_of_pools_ * stats_b.number_of_objects_per_pool_;

        std::vector<TestObject*> drained_b;
        drained_b.reserve(total_capacity_b);

        for (int i = 0; i < total_capacity_b; ++i) {
            TestObject* obj = allocator_b.allocate();
            ASSERT_NE(obj, nullptr) << "allocator B corruption after cross-allocator invalid-free test";
            drained_b.push_back(obj);
        }

        std::unordered_set<TestObject*> seen_b;
        for (auto* obj : drained_b) {
            EXPECT_TRUE(seen_b.insert(obj).second) << "allocator B duplicate pointer after cross-allocator invalid-free test";
        }

        for (auto* obj : drained_b) {
            allocator_b.deallocate(obj);
        }
    }
}

TEST_F(ExpandablePoolAllocatorTest, ConcurrentExpansionFreeListIntegrity) {
    const int objects_per_pool = 4;
    const int initial_pools = 1;
    const int expansion_threshold_hint = 1;

    const int num_threads = 8;
    const int allocations_per_thread = 4; // each thread holds 4

    ExpandablePoolAllocator<TestObject> allocator("ConcurrentExpansionFreeListIntegrity", objects_per_pool, initial_pools, expansion_threshold_hint,
                                                  handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
                                                  UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages));

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    // Each thread will hold its allocations until the end.
    std::vector<std::vector<TestObject*>> per_thread_ptrs(num_threads);

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            auto& local = per_thread_ptrs[t];
            local.reserve(allocations_per_thread);

            for (int i = 0; i < allocations_per_thread; ++i) {
                TestObject* obj = allocator.allocate();
                ASSERT_NE(obj, nullptr) << "allocator returned nullptr in thread " << t << " at allocation " << i;
                local.push_back(obj);
            }

            // Intentionally do NOT deallocate here; we want all slots held
            // concurrently to force multi-pool expansion.
        });
    }

    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    auto stats = allocator.get_pool_statistics();
    EXPECT_GT(stats.number_of_pools_, 1) << "no expansion occurred during deterministic concurrent hold; " << "test did not exercise multi-pool free-list";

    // Now verify free-list integrity by draining and checking uniqueness.
    std::vector<TestObject*> all_ptrs;
    for (auto& v : per_thread_ptrs) {
        all_ptrs.insert(all_ptrs.end(), v.begin(), v.end());
    }
    expect_unique_non_null(all_ptrs, "ConcurrentExpansionFreeListIntegrity");

    for (auto* p : all_ptrs) {
        allocator.deallocate(p);
    }

    auto stats_after = allocator.get_pool_statistics();
    EXPECT_EQ(stats_after.number_of_allocated_objects_, 0);
}

TEST_F(ExpandablePoolAllocatorTest, MixedChaosInvalidFreeStress) {
#ifdef USING_VALGRIND
    const int iterations = 500;
    const int num_threads = 4;
#else
    // There is a balancing act to do here. The larger the number of iterations,
    // the better the test at shaking out potential issues, but the longer it takes
    // to run. 20'000 takes nearly 3 minutes, so we had to reduce it.
    // 10'000 takes around 40s, still far too long.
    // 7'000 takes around 20s, still long but let's put up with that for now.
    const int iterations = 7'000;
    const int num_threads = 8;
#endif

    const int objects_per_pool = 8;
    const int initial_pools = 1;
    const int threshold_hint = 1;

    // Two allocators sharing the same test-level callbacks.
    auto allocator_a = make_allocator("MixedChaos_A", objects_per_pool, initial_pools, threshold_hint);
    auto allocator_b = make_allocator("MixedChaos_B", objects_per_pool, initial_pools, threshold_hint);

    std::atomic<bool> start_gate{false};
    std::atomic<int> ready_count{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::mt19937 rng(static_cast<unsigned>(t + 1));
            std::uniform_int_distribution<int> op_dist(0, 3);

            std::vector<TestObject*> owned_a;
            std::vector<TestObject*> owned_b;
            owned_a.reserve(64);
            owned_b.reserve(64);

            for (int i = 0; i < iterations; ++i) {
                const int op = op_dist(rng);
                if (op == 0) {
                    // allocate from A
                    TestObject* obj = allocator_a.allocate();
                    if (obj != nullptr) {
                        owned_a.push_back(obj);
                    }

                } else if (op == 1) {
                    // allocate from B
                    TestObject* obj = allocator_b.allocate();
                    if (obj != nullptr) {
                        owned_b.push_back(obj);
                    }

                } else if (op == 2) {
                    // valid deallocate from A or B if available
                    if (!owned_a.empty()) {
                        TestObject* obj = owned_a.back();
                        owned_a.pop_back();
                        allocator_a.deallocate(obj);
                    } else if (!owned_b.empty()) {
                        TestObject* obj = owned_b.back();
                        owned_b.pop_back();
                        allocator_b.deallocate(obj);
                    }

                } else if (op == 3) {
                    // invalid frees: stack object and cross-allocator free

                    // Stack object -> invalid free into A
                    TestObject bogus;
                    allocator_a.deallocate(&bogus);

                    // Cross-allocator free: take from A, free into B
                    if (!owned_a.empty()) {
                        TestObject* obj = owned_a.back();
                        owned_a.pop_back();
                        allocator_b.deallocate(obj); // must be detected as invalid
                    }
                }

                if ((static_cast<unsigned>(i) & 0xFFU) == 0) {
                    std::this_thread::yield();
                }
            }
            // Clean up any remaining valid ownership.
            for (auto* obj : owned_a) {
                allocator_a.deallocate(obj);
            }
            for (auto* obj : owned_b) {
                allocator_b.deallocate(obj);
            }
        });
    }

    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    // We must have seen some invalid frees (stack + cross-allocator).
    EXPECT_GT(invalid_free_callback_count_.load(), 0) << "mixed chaos stress did not trigger any invalid-free callbacks";

    // Structural sanity for allocator A.
    {
        auto stats = allocator_a.get_pool_statistics();
        const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

        std::vector<TestObject*> drained;
        drained.reserve(total_capacity);
        for (int i = 0; i < total_capacity; ++i) {
            TestObject* obj = allocator_a.allocate();
            ASSERT_NE(obj, nullptr) << "allocator A corruption after mixed chaos stress at index " << i << " of " << total_capacity;
            drained.push_back(obj);
        }
        expect_unique_non_null(drained, "allocator A mixed chaos drain");
        for (auto* obj : drained) {
            allocator_a.deallocate(obj);
        }
    }

    // Structural sanity for allocator B.
    {
        auto stats = allocator_b.get_pool_statistics();
        const int total_capacity = stats.number_of_pools_ * stats.number_of_objects_per_pool_;

        std::vector<TestObject*> drained;
        drained.reserve(total_capacity);
        for (int i = 0; i < total_capacity; ++i) {
            TestObject* obj = allocator_b.allocate();
            ASSERT_NE(obj, nullptr) << "allocator B corruption after mixed chaos stress at index " << i << " of " << total_capacity;
            drained.push_back(obj);
        }
        expect_unique_non_null(drained, "allocator B mixed chaos drain");
        for (auto* obj : drained) {
            allocator_b.deallocate(obj);
        }
    }
}

TEST_F(ExpandablePoolAllocatorTest, OobScribbleBeforeObjectFragilityProbe) {
#ifdef USING_VALGRIND
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under Valgrind.";
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under ASan.";
#endif
#endif

    const int objects_per_pool = 16;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    auto allocator = make_allocator("OobScribbleBeforeObjectFragilityProbe", objects_per_pool, initial_pools, expansion_threshold);

    // Step 1: allocate a single object.
    TestObject* obj = allocator.allocate();
    ASSERT_NE(obj, nullptr) << "allocator failed initial allocation";

    // Step 2: scribble a few bytes *before* the returned pointer.
    // This simulates the production bug pattern: user writes slightly before
    // the object, into whatever header/metadata the allocator might keep there.
    auto* raw = reinterpret_cast<std::byte*>(obj);
    constexpr int kBytesBefore = 8;
    for (int i = 1; i <= kBytesBefore; ++i) {
        raw[-i] = std::byte{0x5A}; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) UB by design: fragility probe
    }

    // Step 3: try to keep using the allocator.
    // If the allocator stores critical metadata immediately before the object,
    // this is where things will blow up (either now or on subsequent allocs).
    allocator.deallocate(obj);

    // Allocate a full pool and then drain it to check structural integrity.
    std::vector<TestObject*> ptrs;
    ptrs.reserve(objects_per_pool);
    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject* p = allocator.allocate();
        ASSERT_NE(p, nullptr) << "allocator corruption after OOB scribble at index " << i;
        ptrs.push_back(p);
    }

    expect_unique_non_null(ptrs, "OobScribbleBeforeObjectFragilityProbe drain");

    for (auto* p : ptrs) {
        allocator.deallocate(p);
    }
}

// ============================================================================
// Canary corruption tests
//
// These tests verify the behaviour of the allocator when a T object writes
// before its own start address, corrupting the canary that sits between
// is_constructed and the object storage in each Slot<T>.
//
// All three tests are skipped under ASAN and Valgrind because the
// deliberate out-of-bounds write is what those tools are designed to catch —
// they would abort the process before the canary logic even runs.
// ============================================================================

TEST_F(ExpandablePoolAllocatorTest, CanaryCorruptionDeallocateCallsHandler) {
#ifdef USING_VALGRIND
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under Valgrind/TSan.";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under ASan.";
#endif
#endif

    auto allocator = make_allocator("CanaryCorruptionDeallocateCallsHandler", 16);

    TestObject* obj = allocator.allocate();
    ASSERT_NE(obj, nullptr);

    // Corrupt the canary by writing one byte immediately before the object.
    // The canary occupies the 8 bytes before the object in the Slot layout:
    //   [ is_constructed | canary | storage ]
    // A one-byte underrun lands in the canary, not in is_constructed.
    *reinterpret_cast<std::byte*>(obj) = std::byte{0xAB}; // NOLINT — intentional OOB
    reinterpret_cast<std::byte*>(obj)[-1] = std::byte{0xAB}; // NOLINT — intentional OOB

    allocator.deallocate(obj);

    EXPECT_EQ(invalid_free_callback_count_.load(), 1);
}

TEST_F(ExpandablePoolAllocatorTest, CanaryCorruptionDeallocateDoesNotDestruct) {
#ifdef USING_VALGRIND
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under Valgrind/TSan.";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under ASan.";
#endif
#endif

    auto allocator = make_allocator("CanaryCorruptionDeallocateDoesNotDestruct", 16);

    TestObject* obj = allocator.allocate();
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(TestObject::s_constructor_count.load(), 1);

    reinterpret_cast<std::byte*>(obj)[-1] = std::byte{0xAB}; // NOLINT — intentional OOB

    allocator.deallocate(obj);

    // ~TestObject must not have been called — doing so on a potentially
    // corrupt object risks a secondary crash that would obscure the real failure.
    EXPECT_EQ(TestObject::s_destructor_count.load(), 0);
}

TEST_F(ExpandablePoolAllocatorTest, CanaryCorruptionDestructorCallsHandlerAndSkipsDestructor) {
#ifdef USING_VALGRIND
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under Valgrind/TSan.";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    GTEST_SKIP() << "Intentional out-of-bounds scribble; disabled under ASan.";
#endif
#endif

    {
        auto allocator = make_allocator("CanaryCorruptionDestructorCallsHandlerAndSkipsDestructor", 16);

        TestObject* obj_clean   = allocator.allocate();
        TestObject* obj_corrupt = allocator.allocate();
        ASSERT_NE(obj_clean,   nullptr);
        ASSERT_NE(obj_corrupt, nullptr);
        EXPECT_EQ(TestObject::s_constructor_count.load(), 2);

        // Corrupt the canary on one object and leave both unreturned —
        // they become caller-leaked objects discovered by the allocator destructor.
        reinterpret_cast<std::byte*>(obj_corrupt)[-1] = std::byte{0xAB}; // NOLINT — intentional OOB

        // allocator goes out of scope here, triggering ~ExpandablePoolAllocator()
    }

    // The handler must have fired exactly once for the corrupt slot.
    EXPECT_EQ(invalid_free_callback_count_.load(), 1);

    // ~TestObject must have been called for the clean object only.
    EXPECT_EQ(TestObject::s_destructor_count.load(), 1);
}

} // namespace pubsub_itc_fw::tests
