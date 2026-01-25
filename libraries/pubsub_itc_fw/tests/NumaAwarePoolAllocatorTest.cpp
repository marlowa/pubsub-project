#include <gtest/gtest.h>
#include <pthread.h>
#include <numa.h>
#include <sched.h>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>

#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/tests_common/UnitTestLogger.hpp>

namespace pubsub_itc_fw::tests {

// Helper class for NUMA-aware thread pinning
class NumaTopology {
public:
    NumaTopology() {
        if (numa_available() < 0) {
            available_ = false;
            return;
        }
        
        available_ = true;
        num_nodes_ = numa_num_configured_nodes();
        
        // Get CPUs for each NUMA node
        for (int node = 0; node < num_nodes_; ++node) {
            struct bitmask* cpus = numa_allocate_cpumask();
            numa_node_to_cpus(node, cpus);
            
            std::vector<int> node_cpus;
            for (int cpu = 0; cpu < numa_num_configured_cpus(); ++cpu) {
                if (numa_bitmask_isbitset(cpus, cpu)) {
                    node_cpus.push_back(cpu);
                }
            }
            
            numa_free_cpumask(cpus);
            
            if (!node_cpus.empty()) {
                cpus_per_node_.push_back(node_cpus);
            }
        }
    }
    
    bool is_available() const { return available_; }
    int num_nodes() const { return num_nodes_; }
    
    // Get CPUs for a specific NUMA node
    const std::vector<int>& get_cpus_for_node(int node) const {
        return cpus_per_node_[node];
    }
    
    // Find best node with at least min_cpus cores
    int find_best_node(int min_cpus) const {
        for (size_t i = 0; i < cpus_per_node_.size(); ++i) {
            if (static_cast<int>(cpus_per_node_[i].size()) >= min_cpus) {
                return i;
            }
        }
        return -1;
    }
    
    // Pin current thread to specific CPU
    static bool pin_to_cpu(int cpu) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        
        return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
    }
    
private:
    bool available_{false};
    int num_nodes_{0};
    std::vector<std::vector<int>> cpus_per_node_;
};

// Enhanced test fixture with NUMA support
class NumaAwarePoolAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestObject::reset_counts();
        pool_exhausted_callback_count_ = 0;
        invalid_free_callback_count_ = 0;
        huge_pages_error_callback_count_ = 0;
        unit_test_logger_ = std::make_unique<pubsub_itc_fw::tests_common::UnitTestLogger>(LogLevel::Info);
    }
    
    struct TestObject {
        int id_ = 0;
        std::byte padding_[128];
        static std::atomic<int> s_constructor_count;
        static std::atomic<int> s_destructor_count;
        
        TestObject() { s_constructor_count++; }
        ~TestObject() { s_destructor_count++; }
        
        static void reset_counts() {
            s_constructor_count = 0;
            s_destructor_count = 0;
        }
    };
    
    std::atomic<int> pool_exhausted_callback_count_{0};
    std::atomic<int> invalid_free_callback_count_{0};
    std::atomic<int> huge_pages_error_callback_count_{0};
    std::unique_ptr<pubsub_itc_fw::tests_common::UnitTestLogger> unit_test_logger_;
    
    std::function<void(void*, int)> handler_for_pool_exhausted_ =
        [this](void*, int) { this->pool_exhausted_callback_count_++; };
    
    std::function<void(void*, void*)> handler_for_invalid_free_ =
        [this](void*, void*) { this->invalid_free_callback_count_++; };
    
    std::function<void(void*)> handler_for_huge_pages_error_ =
        [this](void*) { this->huge_pages_error_callback_count_++; };
};

// Static member initialization
std::atomic<int> NumaAwarePoolAllocatorTest::TestObject::s_constructor_count(0);
std::atomic<int> NumaAwarePoolAllocatorTest::TestObject::s_destructor_count(0);

TEST_F(NumaAwarePoolAllocatorTest, NumaPinnedThunderingHerd) {
    NumaTopology topo;
    
    if (!topo.is_available()) {
        GTEST_SKIP() << "NUMA not available on this system";
    }
    
    // Use 10 threads - enough for good contention without saturating all cores
    const int target_threads = 10;
    int numa_node = topo.find_best_node(target_threads);
    
    if (numa_node < 0) {
        GTEST_SKIP() << "No NUMA node with at least " << target_threads << " cores found";
    }
    
    const std::vector<int>& cpus = topo.get_cpus_for_node(numa_node);
    const int num_threads = std::min(target_threads, static_cast<int>(cpus.size()));
    
    std::cout << "Running NUMA-pinned thundering herd:\n"
              << "  NUMA node: " << numa_node << "\n"
              << "  Available CPUs on node: " << cpus.size() << "\n"
              << "  Using threads: " << num_threads << "\n";
    
    const int objects_per_pool = 1;  // Maximum stress - one object per pool
    const int initial_pools = 1;
    const int max_pools = num_threads + 10;
    
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "NumaPinnedTest", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);
    
    std::vector<std::thread> threads;
    std::atomic<bool> start_gate{false};
    std::vector<TestObject*> results(num_threads, nullptr);
    std::atomic<int> pin_failures{0};
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i, cpu = cpus[i]]() {
            if (!NumaTopology::pin_to_cpu(cpu)) {
                pin_failures++;
            }
            
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
    
    EXPECT_EQ(pin_failures.load(), 0) << "Some threads failed to pin to CPU";
    
    int success_count = 0;
    for (auto* ptr : results) {
        if (ptr) {
            success_count++;
            allocator.deallocate(ptr);
        }
    }
    
    EXPECT_EQ(success_count, num_threads) 
        << "Expected all " << num_threads << " threads to successfully allocate";
    EXPECT_EQ(pool_exhausted_callback_count_.load(), num_threads - initial_pools)
        << "Expected " << (num_threads - initial_pools) << " pool expansions";
}

TEST_F(NumaAwarePoolAllocatorTest, NumaPinnedContentionStress) {
    NumaTopology topo;
    
    if (!topo.is_available()) {
        GTEST_SKIP() << "NUMA not available on this system";
    }
    
    const int target_threads = 10;
    int numa_node = topo.find_best_node(target_threads);
    
    if (numa_node < 0) {
        GTEST_SKIP() << "No NUMA node with at least " << target_threads << " cores found";
    }
    
    const std::vector<int>& cpus = topo.get_cpus_for_node(numa_node);
    const int num_threads = std::min(target_threads, static_cast<int>(cpus.size()));
    
    std::cout << "Running NUMA-pinned contention stress with " << num_threads << " threads\n";
    
    const int objects_per_pool = 100;
    const int initial_pools = 1;
    const int max_pools = 5;
    const int allocations_per_thread = 200;
    
    ExpandablePoolAllocator<TestObject> allocator(
        *unit_test_logger_, "ContentionStress", objects_per_pool, initial_pools, max_pools,
        handler_for_pool_exhausted_, handler_for_invalid_free_, handler_for_huge_pages_error_,
        UseHugePagesFlag::DoNotUseHugePages);
    
    std::vector<std::thread> threads;
    std::atomic<int> allocation_failures{0};
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, cpu = cpus[i]]() {
            NumaTopology::pin_to_cpu(cpu);
            
            std::vector<TestObject*> my_objects;
            my_objects.reserve(allocations_per_thread);
            
            // Allocate many objects
            for (int j = 0; j < allocations_per_thread; ++j) {
                TestObject* obj = allocator.allocate();
                if (obj) {
                    obj->id_ = j;
                    my_objects.push_back(obj);
                } else {
                    allocation_failures++;
                }
            }
            
            // Deallocate in reverse order
            for (auto it = my_objects.rbegin(); it != my_objects.rend(); ++it) {
                allocator.deallocate(*it);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto stats = allocator.get_pool_statistics();
    EXPECT_EQ(stats.number_of_allocated_objects_, 0) << "Memory leak detected";
    
    // Some allocation failures are expected if we hit max_pools limit
    std::cout << "  Allocation failures: " << allocation_failures.load() 
              << " (expected if pool limit reached)\n";
}

} // namespace pubsub_itc_fw::tests
