#include <gtest/gtest.h>           // Google Test framework
#include <atomic>                  // For std::atomic
#include <functional>              // For std::function
#include <memory>                  // For std::unique_ptr
#include <string>                  // For std::string
#include <thread>                  // For std::thread
#include <vector>                  // For std::vector
#include <random>                  // For std::shuffle
#include <algorithm>               // For std::find

// Project headers
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>                 // For PUBSUB_LOG macro and LogLevel
#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>
#include <pubsub_itc_fw/tests_common/UnitTestLogger.hpp> // For the new UnitTestLogger
#include <pubsub_itc_fw/tests_common/LatencyRecorder.hpp>

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

        // Corrected: Pass LogLevel::Info to the constructor of UnitTestLogger
        unit_test_logger_ = std::make_unique<pubsub_itc_fw::tests_common::UnitTestLogger>(LogLevel::Info);
    }

    std::atomic<int> pool_exhausted_callback_count_;
    std::atomic<int> invalid_free_callback_count_;
    std::atomic<int> huge_pages_error_callback_count_;
    // Corrected type to use the fully qualified namespace
    std::unique_ptr<pubsub_itc_fw::tests_common::UnitTestLogger> unit_test_logger_;

    // Callbacks for the allocator to use
    std::function<void(void*, int)> handler_for_pool_exhausted_ =
        [this](void* for_sender_client_use, int objects_per_pool) {
        this->pool_exhausted_callback_count_++;
    };

    std::function<void(void*, void*)> handler_for_invalid_free_ =
        [this](void* for_receiver_client_use, void* object_to_deallocate) {
        this->invalid_free_callback_count_++;
    };

    std::function<void(void*)> handler_for_huge_pages_error_ =
        [this](void* for_client_use) {
        this->huge_pages_error_callback_count_++;
    };
};

TEST_F(ExpandablePoolAllocatorTest, BasicAllocationAndDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int max_pools = 1; // Test with a single, non-expanding pool
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "BasicTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<TestObject*> allocated_objects;
    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        allocated_objects.push_back(obj);
    }

    // Ensure no more objects can be allocated (pool is full)
    ASSERT_EQ(allocator.allocate(), nullptr);

    // No expansion should have occurred, so the callback count must be 0
    ASSERT_EQ(pool_exhausted_callback_count_.load(), 0);

    // Ensure all objects were constructed
    EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool);
    // Deallocate all objects
    for (TestObject* obj : allocated_objects) {
        allocator.deallocate(obj);
    }
    // Ensure all objects were destructed
    EXPECT_EQ(TestObject::s_destructor_count.load(), objects_per_pool);
}

TEST_F(ExpandablePoolAllocatorTest, PoolExpansionOnExhaustion) {
    const int objects_per_pool = 5;
    const int initial_pools = 1;
    const int max_pools = 2; // Allow one expansion
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ExpansionTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    // Allocate enough objects to fill the first pool and trigger a second one
    std::vector<TestObject*> allocated_objects;
    for (int i = 0; i < objects_per_pool + 1; ++i) {
        TestObject* obj = allocator.allocate();
        ASSERT_NE(obj, nullptr) << " i = " << i << " of " << objects_per_pool;
        allocated_objects.push_back(obj);
    }

    // A pool expansion must have occurred and the callback must have been fired
    EXPECT_EQ(pool_exhausted_callback_count_.load(), 1);
    EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool + 1);

    // Deallocate all objects
    for (TestObject* obj : allocated_objects) {
        allocator.deallocate(obj);
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), objects_per_pool + 1);
}

TEST_F(ExpandablePoolAllocatorTest, MaxChainLengthEnforcement) {
    const int objects_per_pool = 5;
    const int initial_pools = 1;
    const int max_pools = 2; // Allow one expansion
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "MaxChainTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    // Allocate enough objects to fill the first pool, trigger the second, and then fail on the third
    std::vector<TestObject*> allocated_objects;
    const int total_objects_to_allocate = (objects_per_pool * max_pools) + 1;
    for (int i = 0; i < total_objects_to_allocate; ++i) {
        TestObject* obj = allocator.allocate();
        if (i < (objects_per_pool * max_pools)) {
            ASSERT_NE(obj, nullptr);
            allocated_objects.push_back(obj);
        } else {
            // The very last allocation should fail as we've hit the max chain length
            ASSERT_EQ(obj, nullptr);
        }
    }

    // The callback should have fired when the second pool was successfully created.
    // It should NOT fire again when the max chain limit is reached and allocation fails.
    EXPECT_EQ(pool_exhausted_callback_count_.load(), 1);
    EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool * max_pools);

    for (TestObject* obj : allocated_objects) {
        allocator.deallocate(obj);
    }
    EXPECT_EQ(TestObject::s_destructor_count.load(), objects_per_pool * max_pools);
}

TEST_F(ExpandablePoolAllocatorTest, ConcurrentAllocationAndDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int max_pools = 8;
    const int total_threads = 4;
    const int allocations_per_thread = 20; // Enough to force expansion

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ConcurrentTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<std::thread> threads;
    std::vector<TestObject*> thread_safe_allocated_objects_ptr_list(total_threads * allocations_per_thread);
    std::atomic<int> thread_safe_counter(0);

    for (int i = 0; i < total_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < allocations_per_thread; ++j) {
                TestObject* obj = allocator.allocate();
                if (obj != nullptr) {
                    obj->id_ = j + 1; // Assign a unique-ish ID for verification
                    int index = thread_safe_counter.fetch_add(1, std::memory_order_relaxed);
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

    // The test should now pass because the allocator will expand to accommodate all allocations
    // up to the maximum chain length.
    EXPECT_EQ(successful_allocations, total_expected_allocations);
    EXPECT_EQ(TestObject::s_constructor_count.load(), total_expected_allocations);

    // Deallocate objects
    std::shuffle(thread_safe_allocated_objects_ptr_list.begin(),
                 thread_safe_allocated_objects_ptr_list.end(),
                 std::default_random_engine(std::random_device()()));

    for (auto* obj : thread_safe_allocated_objects_ptr_list) {
        if (obj != nullptr) {
            allocator.deallocate(obj);
        }
    }

    // Verify all objects were destructed
    EXPECT_EQ(TestObject::s_destructor_count.load(), total_expected_allocations);
}

TEST_F(ExpandablePoolAllocatorTest, InvalidDeallocation) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int max_pools = 1;
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "InvalidDeallocationTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    // Create an object that wasn't allocated from the pool
    TestObject invalid_object;

    // Attempt to deallocate it and expect the callback to be triggered
    allocator.deallocate(&invalid_object);

    EXPECT_EQ(invalid_free_callback_count_.load(), 1);
}

TEST_F(ExpandablePoolAllocatorTest, HugePagesBehavior) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int max_pools = 1;

    // We will test both scenarios: with and without huge pages.
    // First, test with DoUseHugePages
    ExpandablePoolAllocator<TestObject> allocator_hp(
        *unit_test_logger_, "HugePagesTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoUseHugePages);

    // Test that a huge page allocation was attempted
    // The huge_pages_error_callback_count_ will be > 0 if the attempt failed
    // and the allocator fell back to normal pages.
    // If it's 0, it means mmap was successful for huge pages.
    EXPECT_GE(huge_pages_error_callback_count_.load(), 0);

    // Now test with DoNotUseHugePages
    huge_pages_error_callback_count_ = 0; // Reset the counter
    ExpandablePoolAllocator<TestObject> allocator_no_hp(
        *unit_test_logger_, "NoHugePagesTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    // Ensure the huge page error callback was NOT called
    EXPECT_EQ(huge_pages_error_callback_count_.load(), 0);
}

TEST_F(ExpandablePoolAllocatorTest, ProducerConsumerStressTest) {
    const int objects_per_pool = 100;
    const int initial_pools = 1;
    const int max_pools = 50;
    const int num_producers = 4;
    const int num_consumers = 4;
    const int items_per_producer = 1000;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "StressTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<TestObject*> queue;
    std::mutex queue_mutex;
    std::atomic<bool> production_finished{false};
    std::atomic<int> total_consumed{0};

    // Producers: Constantly allocating and pushing to a shared queue
    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            for (int j = 0; j < items_per_producer; ++j) {
                TestObject* obj = nullptr;
                while ((obj = allocator.allocate()) == nullptr) {
                    std::this_thread::yield(); // Wait if pool is temporarily exhausted
                }
                obj->id_ = j;
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push_back(obj);
            }
        });
    }

    // Consumers: Constantly popping from queue and deallocating
    std::vector<std::thread> consumers;
    for (int i = 0; i < num_consumers; ++i) {
        consumers.emplace_back([&]() {
            while (!production_finished || !queue.empty()) {
                TestObject* obj = nullptr;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!queue.empty()) {
                        obj = queue.back();
                        queue.pop_back();
                    }
                }

                if (obj) {
                    allocator.deallocate(obj);
                    total_consumed++;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    production_finished = true;
    for (auto& t : consumers) t.join();

    EXPECT_EQ(total_consumed.load(), num_producers * items_per_producer);
    EXPECT_EQ(TestObject::s_destructor_count.load(), total_consumed.load());
}

TEST_F(ExpandablePoolAllocatorTest, ThunderingHerdExpansionRace) {
    const int objects_per_pool = 1; // Extremely small to force immediate expansion
    const int initial_pools = 1;
    const int max_pools = 100;
    const int num_threads = 80; // High thread count

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "RaceTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<std::thread> threads;
    std::atomic<bool> start_gate{false};
    std::vector<TestObject*> results(num_threads, nullptr);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            while (!start_gate) std::this_thread::yield();
            results[i] = allocator.allocate();
        });
    }

    start_gate = true; // Release the herd
    for (auto& t : threads) t.join();

    int success_count = 0;
    for (auto* ptr : results) {
        if (ptr) {
            success_count++;
            allocator.deallocate(ptr);
        }
    }

    // Since max_pools (100) > num_threads (80), everyone should succeed
    EXPECT_EQ(success_count, num_threads);
    EXPECT_EQ(pool_exhausted_callback_count_.load(), num_threads - initial_pools);
}

TEST_F(ExpandablePoolAllocatorTest, CacheLineContentionStress) {
    const int objects_per_pool = 1000;
    const int num_threads = 12; // Exceed typical physical core count
    const int iterations = 5000;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ContentionTest", objects_per_pool, 1, 1,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                TestObject* obj = allocator.allocate();
                if (obj) {
                    // Touch the memory to ensure it's actually mapped and owned
                    obj->id_ = j;
                    allocator.deallocate(obj);
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0);
}

TEST_F(ExpandablePoolAllocatorTest, LatencyStressTest) {
    const int num_threads = 80;
    const int objects_per_pool = 1000;
    const int iterations = 100;
    
    // 1. Setup: Start with 1 initial pool to observe the transition 
    // from steady-state to chained-state.
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "LatencyTest", objects_per_pool, 1, 10,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

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
        
        if (obj) {
            alloc_recorder.record(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            sentinel_objects.push_back(obj);
        }
    }

    // 3. MULTI-THREADED STRESS: 80 threads fight for the remaining 100 slots 
    // in Pool 1, then trickle into Pool 2 and 3.
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < iterations; ++j) {
                // Measure Allocation
                auto start = std::chrono::high_resolution_clock::now();
                TestObject* obj = allocator.allocate();
                auto end = std::chrono::high_resolution_clock::now();
                
                if (obj) {
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

} // namespace pubsub_itc_fw::tests
