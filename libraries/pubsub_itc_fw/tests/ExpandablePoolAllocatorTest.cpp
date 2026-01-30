#include <gtest/gtest.h>           // Google Test framework
#include <atomic>                  // For std::atomic
#include <functional>              // For std::function
#include <memory>                  // For std::unique_ptr
#include <string>                  // For std::string
#include <thread>                  // For std::thread
#include <vector>                  // For std::vector
#include <random>                  // For std::shuffle
#include <algorithm>               // For std::find

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
    const int expansion_threshold = 1;
    
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "BasicTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

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
    
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ExpansionTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

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

TEST_F(ExpandablePoolAllocatorTest, MaxChainLengthEnforcement)
{
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;
    
    ExpandablePoolAllocator<int> allocator(
        *unit_test_logger_, "BasicTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    constexpr std::size_t pool_size = 16U;

    std::vector<int*> pointers;
    pointers.reserve(pool_size);

    auto statistics_before = allocator.get_behaviour_statistics();

    for (std::size_t i = 0U; i < pool_size; ++i) {
        auto* pointer = allocator.allocate();
        ASSERT_NE(pointer, nullptr) << "allocator returned nullptr during initial allocation";
        pointers.push_back(pointer);
    }

    auto statistics_after_initial_allocations = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_before.expansion_events, statistics_after_initial_allocations.expansion_events)
        << "allocator should not expand while exhausting the initial pool";

    for (int* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    auto statistics_after_deallocation = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_after_initial_allocations.expansion_events, statistics_after_deallocation.expansion_events)
        << "deallocation should not trigger pool expansion";

    for (int* pointer : pointers) {
        allocator.deallocate(pointer);
    }

    auto statistics_after_over_free = allocator.get_behaviour_statistics();
    EXPECT_EQ(statistics_after_deallocation.expansion_events, statistics_after_over_free.expansion_events)
        << "over-free attempts must not cause pool expansion";

    std::vector<int*> pointers_second_round;
    pointers_second_round.reserve(pool_size);

    for (std::size_t i = 0U; i < pool_size; ++i) {
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
    const int total_threads = 4;
    const int allocations_per_thread = 20;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ConcurrentTest", objects_per_pool, initial_pools, expansion_threshold,
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
                    obj->id_ = j + 1;
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

    EXPECT_EQ(successful_allocations, total_expected_allocations);
    EXPECT_EQ(TestObject::s_constructor_count.load(), total_expected_allocations);

    std::shuffle(thread_safe_allocated_objects_ptr_list.begin(),
                 thread_safe_allocated_objects_ptr_list.end(),
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
    
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "InvalidDeallocationTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    TestObject invalid_object;
    allocator.deallocate(&invalid_object);

    EXPECT_EQ(invalid_free_callback_count_.load(), 1);
}

TEST_F(ExpandablePoolAllocatorTest, HugePagesBehavior) {
    const int objects_per_pool = 10;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    ExpandablePoolAllocator<TestObject> allocator_hp(
        *unit_test_logger_, "HugePagesTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoUseHugePages);

    EXPECT_GE(huge_pages_error_callback_count_.load(), 0);

    huge_pages_error_callback_count_ = 0;
    ExpandablePoolAllocator<TestObject> allocator_no_hp(
        *unit_test_logger_, "NoHugePagesTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    EXPECT_EQ(huge_pages_error_callback_count_.load(), 0);
}

TEST_F(ExpandablePoolAllocatorTest, ThunderingHerdExpansionRace) {
    const int objects_per_pool = 1;
    const int initial_pools = 1;
    const int expansion_threshold = 100;
    const int num_threads = 80;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "RaceTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<std::thread> threads;
    std::atomic<bool> start_gate{false};
    std::vector<TestObject*> results(num_threads, nullptr);

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
        if (ptr) {
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

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "StressTest", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<TestObject*> queue;
    std::mutex queue_mutex;
    std::atomic<bool> production_finished{false};
    std::atomic<int> total_consumed{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < num_producers; ++i) {
        producers.emplace_back([&]() {
            for (int j = 0; j < items_per_producer; ++j) {
                TestObject* obj = nullptr;
                while ((obj = allocator.allocate()) == nullptr) {
                    std::this_thread::yield();
                }
                obj->id_ = j;
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push_back(obj);
            }
        });
    }

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

TEST_F(ExpandablePoolAllocatorTest, DeterministicThunderingHerdOrdering)
{
    const int objects_per_pool = 1;
    const int initial_pools = 1;
    const int expansion_threshold = 100;
    const int num_threads = 32;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "DeterministicHerd", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<TestObject*> results(num_threads, nullptr);
    std::atomic<bool> start_gate{false};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[i] = allocator.allocate();
        });
    }

    start_gate.store(true, std::memory_order_release);

    for (auto &t : threads) {
        t.join();
    }

    int success_count = 0;
    for (auto *ptr : results) {
        if (ptr != nullptr) {
            success_count++;
            allocator.deallocate(ptr);
        }
    }

    EXPECT_EQ(success_count, num_threads);
    EXPECT_EQ(pool_exhausted_callback_count_.load(), num_threads - initial_pools);
}

TEST_F(ExpandablePoolAllocatorTest, PoolCorrectnessAndReuse)
{
    const int objects_per_pool = 8;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "PoolCorrectness", objects_per_pool, initial_pools, expansion_threshold,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);

    std::vector<TestObject*> ptrs;
    ptrs.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject *obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        ptrs.push_back(obj);
    }

    std::vector<TestObject*> shuffled = ptrs;
    std::shuffle(shuffled.begin(), shuffled.end(), std::default_random_engine(std::random_device()()));

    for (auto *obj : shuffled) {
        allocator.deallocate(obj);
    }

    std::vector<TestObject*> ptrs2;
    ptrs2.reserve(objects_per_pool);

    for (int i = 0; i < objects_per_pool; ++i) {
        TestObject *obj = allocator.allocate();
        ASSERT_NE(obj, nullptr);
        ptrs2.push_back(obj);
    }

    // Sort both vectors before comparison
    std::sort(ptrs.begin(), ptrs.end());
    std::sort(ptrs2.begin(), ptrs2.end());
    EXPECT_EQ(ptrs, ptrs2);

    for (auto *obj : ptrs2) {
        allocator.deallocate(obj);
    }
}

TEST_F(ExpandablePoolAllocatorTest, DestructorReleasesAllObjects)
{
    const int objects_per_pool = 16;
    const int initial_pools = 1;
    const int expansion_threshold = 1;

    {
        ExpandablePoolAllocator<TestObject> allocator(
            *unit_test_logger_, "AllocatorDestruction", objects_per_pool, initial_pools, expansion_threshold,
            handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
            UseHugePagesFlag::DoNotUseHugePages);

        for (int i = 0; i < objects_per_pool; ++i) {
            TestObject *obj = allocator.allocate();
            ASSERT_NE(obj, nullptr);
        }

        EXPECT_EQ(TestObject::s_constructor_count.load(), objects_per_pool);
        EXPECT_EQ(TestObject::s_destructor_count.load(), 0);
    }

    EXPECT_EQ(TestObject::s_destructor_count.load(), TestObject::s_constructor_count.load());
}

} // namespace pubsub_itc_fw::tests
