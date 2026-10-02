// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "SessionThrottles.hpp"
#include "ThrottleLimits.hpp"
#include "ThrottleOutcome.hpp"
#include "ThrottledCommand.hpp"

/*
 * Checks that deciding about a command never uses the heap.
 *
 * The global operator new is replaced, for this whole test program, by one that counts the
 * allocations made on the calling thread while counting is switched on, and otherwise behaves as
 * the standard one does. That is why these tests are in a program of their own.
 *
 * The first test makes an allocation on purpose and requires it to be counted. Without it, a
 * counter that had stopped counting -- because the replacement was not linked in, say -- would let
 * every other test here pass while measuring nothing.
 */

namespace {

thread_local bool counting_allocations = false;
thread_local long allocations_counted = 0;

void* allocate(size_t bytes, size_t alignment) {
    if (counting_allocations) {
        ++allocations_counted;
    }
    if (bytes == 0) {
        bytes = 1;
    }
    void* memory = nullptr;
    if (alignment <= alignof(std::max_align_t)) {
        memory = std::malloc(bytes);
    } else {
        // aligned_alloc requires the size to be a multiple of the alignment.
        memory = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
    }
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

/// Counts the allocations made on this thread for as long as it exists.
class AllocationCounter {
  public:
    ~AllocationCounter() {
        counting_allocations = false;
    }

    AllocationCounter() {
        allocations_counted = 0;
        counting_allocations = true;
    }

    AllocationCounter(const AllocationCounter& other) = delete;
    AllocationCounter& operator=(const AllocationCounter& other) = delete;

    [[nodiscard]] long count() const {
        return allocations_counted;
    }
};

} // namespaces

void* operator new(size_t bytes) {
    return allocate(bytes, alignof(std::max_align_t));
}

void* operator new[](size_t bytes) {
    return allocate(bytes, alignof(std::max_align_t));
}

void* operator new(size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<size_t>(alignment));
}

void* operator new[](size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<size_t>(alignment));
}

void operator delete(void* memory) {
    std::free(memory);
}

void operator delete[](void* memory) {
    std::free(memory);
}

void operator delete(void* memory, size_t) {
    std::free(memory);
}

void operator delete[](void* memory, size_t) {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) {
    std::free(memory);
}

void operator delete(void* memory, size_t, std::align_val_t) {
    std::free(memory);
}

void operator delete[](void* memory, size_t, std::align_val_t) {
    std::free(memory);
}

namespace fix_common::tests {

namespace {

using Clock = SessionThrottles::Clock;

const Clock::time_point start_time = Clock::time_point{} + std::chrono::hours(1000);

} // namespaces

TEST(ThrottleHeapUseTest, TheCounterCountsAnAllocationMadeOnPurpose) {
    const AllocationCounter counter;
    std::vector<int> values;
    values.reserve(100);
    values.push_back(1);
    EXPECT_GE(counter.count(), 1) << "the replacement operator new is not counting, so no other test here measures anything";
}

TEST(ThrottleHeapUseTest, CreatingTheThrottlesIsWhereTheyAllocate) {
    ThrottleLimits limits;
    limits.max_place_per_second = 50;
    limits.max_amend_per_second = 10;
    limits.max_cancel_per_second = 50;
    const AllocationCounter counter;
    const SessionThrottles throttles(limits);
    EXPECT_GE(counter.count(), 3) << "three throttles with limits were created without allocating their storage";
}

TEST(ThrottleHeapUseTest, DecidingAboutCommandsNeverUsesTheHeap) {
    ThrottleLimits limits;
    limits.max_place_per_second = 50;
    limits.max_amend_per_second = 0;
    limits.max_cancel_per_second = 7;
    SessionThrottles throttles(limits);

    // Commands every 3 ms of each kind for 100 seconds of simulated time: well over the limits, so
    // every outcome occurs, every refusal text is read, and the ring buffers wrap many times.
    long accepted = 0;
    long refused = 0;
    const AllocationCounter counter;
    for (int step = 0; step < 33000; ++step) {
        const Clock::time_point now = start_time + std::chrono::milliseconds(step * 3);
        for (const ThrottledCommand command : {ThrottledCommand::Place, ThrottledCommand::Amend, ThrottledCommand::Cancel}) {
            if (is_accepted(throttles.try_accept(command, now))) {
                ++accepted;
            } else if (!throttles.refusal_text(command).empty()) {
                ++refused;
            }
        }
    }
    const long allocations = counter.count();

    EXPECT_EQ(allocations, 0);
    EXPECT_GT(accepted, 0);
    EXPECT_GT(refused, 0);
}

} // namespaces
