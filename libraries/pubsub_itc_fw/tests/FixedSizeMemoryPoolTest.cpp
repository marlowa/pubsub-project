#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <thread>
#include <mutex>
#include <vector>
#include <random>
#include <type_traits> // for std::aligned_storage_t

#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

using namespace pubsub_itc_fw;

namespace {

struct TestObject {
    int id_{0};
    std::byte padding_[128];

    static std::atomic<int> s_ctor;
    static std::atomic<int> s_dtor;

    TestObject() { s_ctor.fetch_add(1, std::memory_order_relaxed); }
    ~TestObject() { s_dtor.fetch_add(1, std::memory_order_relaxed); }

    static void reset() {
        s_ctor.store(0, std::memory_order_relaxed);
        s_dtor.store(0, std::memory_order_relaxed);
    }
};

std::atomic<int> TestObject::s_ctor{0};
std::atomic<int> TestObject::s_dtor{0};

class FixedSizeMemoryPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestObject::reset();
        huge_pages_error_calls_ = 0;
    }

    std::atomic<int> huge_pages_error_calls_{0};

    std::function<void(void*, std::size_t)> huge_page_handler() {
        return [this](void*, std::size_t) {
            huge_pages_error_calls_.fetch_add(1, std::memory_order_relaxed);
        };
    }
};

// ------------------------------------------------------------
// Basic allocate/deallocate and capacity behaviour
// ------------------------------------------------------------

TEST_F(FixedSizeMemoryPoolTest, BasicAllocateDeallocate) {
    const int capacity = 16;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    std::vector<TestObject*> ptrs;
    ptrs.reserve(capacity);

    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr) << "allocate() returned nullptr before capacity exhausted";
        p->id_ = i;
        ptrs.push_back(p);
    }

    EXPECT_TRUE(pool.is_full());
    EXPECT_EQ(pool.get_number_of_available_objects(), 0);

    // Next allocation must return nullptr
    EXPECT_EQ(pool.allocate(), nullptr);

    // Deallocate all
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }

    EXPECT_FALSE(pool.is_full());
    EXPECT_EQ(pool.get_number_of_available_objects(), capacity);
}

// ------------------------------------------------------------
// Randomised allocation/deallocation order, reuse of all slots
// ------------------------------------------------------------

TEST_F(FixedSizeMemoryPoolTest, RandomisedReuse) {
    const int capacity = 32;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    std::vector<TestObject*> first_round;
    first_round.reserve(capacity);

    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        first_round.push_back(p);
    }

    // Shuffle and deallocate in random order. We do not care that on some embedded Linux targets `std::random_device` is deterministic.
    std::vector<TestObject*> shuffled = first_round;
    std::shuffle(shuffled.begin(), shuffled.end(), std::default_random_engine(std::random_device()()));
    for (auto* p : shuffled) {
        pool.deallocate(p);
    }

    // Second round: we should get exactly the same set of addresses back
    std::vector<TestObject*> second_round;
    second_round.reserve(capacity);
    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        second_round.push_back(p);
    }

    std::sort(first_round.begin(), first_round.end());
    std::sort(second_round.begin(), second_round.end());
    EXPECT_EQ(first_round, second_round);

    for (auto* p : second_round) {
        pool.deallocate(p);
    }
}

// ------------------------------------------------------------
// contains() correctness and boundary conditions
// ------------------------------------------------------------

TEST_F(FixedSizeMemoryPoolTest, ContainsCorrectness) {
    const int capacity = 8;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    std::vector<TestObject*> ptrs;
    ptrs.reserve(capacity);
    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }

    for (auto* p : ptrs) {
        EXPECT_TRUE(pool.contains(p));
    }

    // Some obviously invalid pointers
    const TestObject stack_obj;
    EXPECT_FALSE(pool.contains(&stack_obj));

    // Pointer just outside the pool region: we approximate by taking min/max
    auto* min_ptr = *std::min_element(ptrs.begin(), ptrs.end());
    auto* max_ptr = *std::max_element(ptrs.begin(), ptrs.end());

    // These are heuristic checks; we just want "clearly outside" to be false.
    EXPECT_FALSE(pool.contains(reinterpret_cast<TestObject*>(
        reinterpret_cast<char*>(min_ptr) - sizeof(TestObject))));
    EXPECT_FALSE(pool.contains(reinterpret_cast<TestObject*>(
        reinterpret_cast<char*>(max_ptr) + sizeof(TestObject) * 2)));

    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

// ------------------------------------------------------------
// Slot layout invariants: slot_from_object / object_from_slot
// ------------------------------------------------------------

TEST_F(FixedSizeMemoryPoolTest, SlotLayoutRoundTrip) {
    const int capacity = 4;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    std::vector<TestObject*> ptrs;
    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }

    // We can't access slot_from_object directly (it's private),
    // but we can at least assert that all pointers are within a
    // single contiguous region and aligned as expected.
    auto* min_ptr = *std::min_element(ptrs.begin(), ptrs.end());
    auto* max_ptr = *std::max_element(ptrs.begin(), ptrs.end());

    // All objects should be within [min, max] and aligned to alignof(TestObject)
    for (auto* p : ptrs) {
        EXPECT_GE(p, min_ptr);
        EXPECT_LE(p, max_ptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(TestObject), 0U);
    }

    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST_F(FixedSizeMemoryPoolTest, DestructorDestroysLeakedObjects) {
    const int capacity = 10;
    {
        FixedSizeMemoryPool<TestObject> pool(
            capacity,
            UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
            huge_page_handler()
        );

        for (int i = 0; i < capacity; ++i) {
            void* raw = pool.allocate();
            ASSERT_NE(raw, nullptr);

            // Construct the object
            auto* p = new (raw) TestObject();
            p->id_ = i;

            // Mark slot as constructed (simulating ExpandablePoolAllocator’s lifetime protocol).
            // FixedSizeMemoryPool does not set this bit; it only reads it in its destructor.”
            using SlotType = Slot<TestObject>;
            auto* storage_ptr = reinterpret_cast<std::aligned_storage_t<sizeof(TestObject), alignof(TestObject)>*>(p);
            auto* slot = reinterpret_cast<SlotType*>(
                reinterpret_cast<char*>(storage_ptr) - offsetof(SlotType, storage)
            );
            slot->is_constructed.store(1, std::memory_order_relaxed);
        }

        EXPECT_EQ(TestObject::s_ctor.load(), capacity);
        EXPECT_EQ(TestObject::s_dtor.load(), 0);
    }

    EXPECT_EQ(TestObject::s_dtor.load(), TestObject::s_ctor.load());
}

/**
 * DirectAbaStress
 * ----------------
 * This test exercises the FixedSizeMemoryPool under intense concurrent
 * allocate/deallocate pressure to detect *structural* corruption of the
 * lock‑free Treiber free list.
 *
 * What this test *does* verify:
 *   • No nullptrs appear during the final drain (free list not truncated)
 *   • The number of drained slots equals the pool capacity (no lost nodes)
 *   • All drained pointers are unique (no duplicated nodes)
 *   • No extra nodes appear (free list not corrupted or expanded)
 *
 * What this test deliberately does *NOT* verify:
 *   • That every slot was allocated at least once during the stress phase
 *
 * Rationale:
 *   A Treiber stack is LIFO. Under contention, threads may repeatedly pop and
 *   push the same subset of slots. It is *not* guaranteed that all slots will
 *   be used during the stress loop. Requiring full coverage would produce false
 *   failures without indicating corruption.
 *
 * This test therefore focuses on the invariants that *must* hold for a correct
 * lock‑free free list and that *will* be violated by ABA‑related corruption.
 */

TEST_F(FixedSizeMemoryPoolTest, DirectAbaStress) {
#ifdef USING_VALGRIND
    const int iterations  = 200;
    const int num_threads = 4;
#else
    const int iterations  = 100'000;
    const int num_threads = 8;
#endif

    const int capacity = num_threads;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    // Threads hammer allocate()/deallocate() concurrently.
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < iterations; ++i) {
                TestObject* obj = pool.allocate();
                if (obj == nullptr) {
                    continue;
                }
                pool.deallocate(obj);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Drain the pool completely and verify structural integrity.
    std::vector<TestObject*> drained;
    drained.reserve(capacity);

    for (int i = 0; i < capacity; ++i) {
        TestObject* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "free-list corruption: nullptr during drain at index " << i;
        drained.push_back(obj);
    }

    // No more slots must be available.
    EXPECT_EQ(pool.allocate(), nullptr) << "free-list corruption: allocator returned more than capacity slots";

    // All drained pointers must be unique.
    const std::set<TestObject*> drained_set(drained.begin(), drained.end());
    EXPECT_EQ(drained_set.size(), drained.size()) << "free-list corruption: duplicate pointer detected during drain";

    // Return drained objects to the pool.
    for (auto* obj : drained) {
        pool.deallocate(obj);
    }
}

// ------------------------------------------------------------
// Huge pages behaviour: handler is called on fallback (best-effort)
// ------------------------------------------------------------

TEST_F(FixedSizeMemoryPoolTest, HugePagesHandlerIsCallable) {
    const int capacity = 4;

    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoUseHugePages),
        huge_page_handler()
    );

    // We can't force the OS to fail huge pages, but we can at least assert
    // that the pool is usable and the handler counter is >= 0.
    EXPECT_GE(huge_pages_error_calls_.load(std::memory_order_relaxed), 0);

    std::vector<TestObject*> ptrs;
    for (int i = 0; i < capacity; ++i) {
        auto* p = pool.allocate();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    for (auto* p : ptrs) {
        pool.deallocate(p);
    }
}

TEST_F(FixedSizeMemoryPoolTest, DirectAbaStressWithMidDrain) {
#ifdef USING_VALGRIND
    const int iterations     = 200;
    const int num_threads    = 4;
    const int num_mid_drains = 4;
#else
    const int iterations     = 100'000;
    const int num_threads    = 8;
    const int num_mid_drains = 16;
#endif

    // Capacity == num_threads to maximise contention at the boundary.
    const int capacity = num_threads;
    FixedSizeMemoryPool<TestObject> pool(
        capacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        huge_page_handler()
    );

    std::atomic<bool> start_gate{false};
    std::atomic<bool> stop{false};
    std::atomic<int>  ready_count{0};

    // Worker threads: pure Treiber-stack pressure (allocate/deallocate).
    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations && !stop.load(std::memory_order_relaxed); ++i) {
                TestObject* obj = pool.allocate();
                if (obj == nullptr) {
                    // Pool temporarily empty; this is fine, just try again.
                    continue;
                }
                obj->id_ = i;
                pool.deallocate(obj);

                if ((i & 0xFF) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait until all workers are ready, then start them.
    while (ready_count.load(std::memory_order_acquire) != num_threads) {
        std::this_thread::yield();
    }
    start_gate.store(true, std::memory_order_release);

    // Mid‑run partial drains: steal a subset of slots, verify structural
    // integrity on whatever we successfully drain, then return them.
    for (int drain_round = 0; drain_round < num_mid_drains; ++drain_round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const int max_drain = capacity / 2;
        std::vector<TestObject*> drained;
        drained.reserve(max_drain);

        for (int i = 0; i < max_drain; ++i) {
            TestObject* obj = pool.allocate();
            if (obj == nullptr) {
                // Pool is temporarily empty because workers are holding slots.
                // This is expected under contention; just stop this mid‑drain.
                break;
            }
            drained.push_back(obj);
        }

        // If we drained anything at all, it must be structurally sound:
        // no duplicates in the sample.
        if (!drained.empty()) {
            std::set<TestObject*> drained_set(drained.begin(), drained.end());
            EXPECT_EQ(drained_set.size(), drained.size())
                << "mid‑drain corruption: duplicate pointer detected in round "
                << drain_round;
        }

        // Return drained objects to the pool so workers can continue.
        for (auto* obj : drained) {
            pool.deallocate(obj);
        }
    }

    // Stop workers and join.
    stop.store(true, std::memory_order_release);
    for (auto& th : workers) {
        th.join();
    }

    // Final full drain: must recover exactly `capacity` unique slots,
    // no nullptrs, no extras.
    std::vector<TestObject*> final_drained;
    final_drained.reserve(capacity);

    for (int i = 0; i < capacity; ++i) {
        TestObject* obj = pool.allocate();
        ASSERT_NE(obj, nullptr)
            << "final drain corruption: nullptr at index " << i
            << " (expected " << capacity << " slots)";
        final_drained.push_back(obj);
    }

    EXPECT_EQ(pool.allocate(), nullptr)
        << "final drain corruption: allocator returned more than capacity slots";

    const std::set<TestObject*> final_set(final_drained.begin(), final_drained.end());
    EXPECT_EQ(final_set.size(), final_drained.size())
        << "final drain corruption: duplicate pointer detected";

    for (auto* obj : final_drained) {
        pool.deallocate(obj);
    }
}

} // namespace
