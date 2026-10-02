// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw::tests {

namespace {

/**
 * @brief An element type that counts its constructions and destructions.
 *
 * The buffer constructs and destroys elements by hand in raw storage, so "every element added is
 * destroyed exactly once" has to be measured. It has no default constructor, which also shows the
 * buffer never needs one.
 */
struct CountedElement {
    static int constructions;
    static int destructions;

    static void reset_counts() {
        constructions = 0;
        destructions = 0;
    }

    static int live_count() {
        return constructions - destructions;
    }

    ~CountedElement() {
        ++destructions;
    }

    explicit CountedElement(int payload_value) : payload(payload_value) {
        ++constructions;
    }

    CountedElement(const CountedElement& other) : payload(other.payload) {
        ++constructions;
    }

    CountedElement(CountedElement&& other) : payload(other.payload) {
        ++constructions;
    }

    CountedElement& operator=(const CountedElement& other) = delete;
    CountedElement& operator=(CountedElement&& other) = delete;

    int payload;
};

int CountedElement::constructions = 0;
int CountedElement::destructions = 0;

/// An element type that must sit on a 64-byte boundary, more than the plain operator new promises.
struct alignas(64) OverAlignedElement {
    explicit OverAlignedElement(int payload_value) : payload(payload_value) {}

    int payload;
};

/// An element type whose constructor throws when given a negative value.
struct ThrowingElement {
    explicit ThrowingElement(int payload_value) : payload(payload_value) {
        if (payload_value < 0) {
            throw std::runtime_error("ThrowingElement refused a negative value");
        }
    }

    int payload;
};

/// Move-assigns through a function, so that a self-move in a test is not visible to the compiler
/// as one: gcc diagnoses a literal `x = std::move(x)` and the build treats warnings as errors.
template <typename BufferType> void move_assign(BufferType& target, BufferType& source) {
    target = std::move(source);
}

} // namespaces

class FixedCapacityRingBufferTest : public ::testing::Test {
  protected:
    void SetUp() override {
        CountedElement::reset_counts();
    }
};

TEST_F(FixedCapacityRingBufferTest, IsEmptyWhenCreatedWithTheCapacityItWasGiven) {
    const FixedCapacityRingBuffer<int> buffer(5);
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.full());
    EXPECT_EQ(buffer.size(), 0u);
    EXPECT_EQ(buffer.capacity(), 5u);
    EXPECT_EQ(buffer.allocated_bytes(), 5u * sizeof(int));
}

TEST_F(FixedCapacityRingBufferTest, ACapacityOfZeroIsRefused) {
    EXPECT_THROW(FixedCapacityRingBuffer<int>(0), PreconditionAssertion);
}

TEST_F(FixedCapacityRingBufferTest, ElementsLeaveInTheOrderTheyArrived) {
    FixedCapacityRingBuffer<int> buffer(4);
    ASSERT_TRUE(buffer.push_back(10));
    ASSERT_TRUE(buffer.push_back(20));
    ASSERT_TRUE(buffer.push_back(30));
    EXPECT_EQ(buffer.size(), 3u);

    EXPECT_EQ(buffer.front(), 10);
    buffer.pop_front();
    EXPECT_EQ(buffer.front(), 20);
    buffer.pop_front();
    EXPECT_EQ(buffer.front(), 30);
    buffer.pop_front();
    EXPECT_TRUE(buffer.empty());
}

TEST_F(FixedCapacityRingBufferTest, AddingToAFullBufferFailsAndChangesNothing) {
    FixedCapacityRingBuffer<int> buffer(3);
    ASSERT_TRUE(buffer.push_back(1));
    ASSERT_TRUE(buffer.push_back(2));
    ASSERT_TRUE(buffer.push_back(3));
    ASSERT_TRUE(buffer.full());

    EXPECT_FALSE(buffer.push_back(4));
    EXPECT_FALSE(buffer.emplace_back(5));
    EXPECT_EQ(buffer.size(), 3u);
    EXPECT_TRUE(buffer.full());

    // Nothing was overwritten: the original three come out, in order.
    EXPECT_EQ(buffer.front(), 1);
    buffer.pop_front();
    EXPECT_EQ(buffer.front(), 2);
    buffer.pop_front();
    EXPECT_EQ(buffer.front(), 3);
}

TEST_F(FixedCapacityRingBufferTest, AfterOneRemovalExactlyOneMoreCanBeAdded) {
    FixedCapacityRingBuffer<int> buffer(3);
    ASSERT_TRUE(buffer.push_back(1));
    ASSERT_TRUE(buffer.push_back(2));
    ASSERT_TRUE(buffer.push_back(3));

    buffer.pop_front();
    EXPECT_TRUE(buffer.push_back(4));
    EXPECT_FALSE(buffer.push_back(5));

    std::vector<int> drained;
    while (!buffer.empty()) {
        drained.push_back(buffer.front());
        buffer.pop_front();
    }
    EXPECT_EQ(drained, (std::vector<int>{2, 3, 4}));
}

TEST_F(FixedCapacityRingBufferTest, ARefusedMoveLeavesTheValueWhereItWas) {
    FixedCapacityRingBuffer<std::string> buffer(1);
    ASSERT_TRUE(buffer.push_back(std::string("first")));

    std::string refused("a string long enough that moving it would take its storage");
    EXPECT_FALSE(buffer.push_back(std::move(refused)));
    EXPECT_EQ(refused, "a string long enough that moving it would take its storage");
}

TEST_F(FixedCapacityRingBufferTest, StaysCorrectAfterWrappingManyTimesAtEveryCapacityFromOneUpwards) {
    // The buffer is compared against std::deque through a long random run of additions and
    // removals at each capacity, so the positions cross the end of the storage many times, from
    // every starting point, with the buffer at every level of fullness.
    std::mt19937 random_engine(20261002U);
    for (size_t capacity = 1; capacity <= 17; ++capacity) {
        FixedCapacityRingBuffer<int> buffer(capacity);
        std::deque<int> expected;
        int next_value = 0;
        for (int step = 0; step < 5000; ++step) {
            const bool add = (random_engine() % 2U) == 0U;
            if (add) {
                const bool added = buffer.push_back(next_value);
                ASSERT_EQ(added, expected.size() < capacity) << "capacity " << capacity << ", step " << step;
                if (added) {
                    expected.push_back(next_value);
                }
                ++next_value;
            } else if (!expected.empty()) {
                ASSERT_EQ(buffer.front(), expected.front()) << "capacity " << capacity << ", step " << step;
                buffer.pop_front();
                expected.pop_front();
            }
            ASSERT_EQ(buffer.size(), expected.size());
            ASSERT_EQ(buffer.full(), expected.size() == capacity);
            ASSERT_EQ(buffer.empty(), expected.empty());
        }
    }
}

TEST_F(FixedCapacityRingBufferTest, StaysCorrectWhenAlwaysFullAcrossManyWraps) {
    // The pattern a throttle under steady load produces: full, remove the oldest, add one.
    const size_t capacity = 7;
    FixedCapacityRingBuffer<int> buffer(capacity);
    int next_value = 0;
    for (size_t count = 0; count < capacity; ++count) {
        ASSERT_TRUE(buffer.push_back(next_value++));
    }
    for (int step = 0; step < 1000; ++step) {
        ASSERT_EQ(buffer.front(), next_value - static_cast<int>(capacity));
        buffer.pop_front();
        ASSERT_TRUE(buffer.push_back(next_value++));
        ASSERT_FALSE(buffer.push_back(-1));
    }
}

TEST_F(FixedCapacityRingBufferTest, AnElementTypeWithNoDefaultConstructorWorks) {
    FixedCapacityRingBuffer<CountedElement> buffer(2);
    ASSERT_TRUE(buffer.emplace_back(7));
    ASSERT_TRUE(buffer.push_back(CountedElement(8)));
    EXPECT_EQ(buffer.front().payload, 7);
    buffer.pop_front();
    EXPECT_EQ(buffer.front().payload, 8);
}

TEST_F(FixedCapacityRingBufferTest, CreatingABufferConstructsNoElements) {
    const FixedCapacityRingBuffer<CountedElement> buffer(100);
    EXPECT_EQ(CountedElement::constructions, 0);
}

TEST_F(FixedCapacityRingBufferTest, EveryElementRemovedIsDestroyedExactlyOnce) {
    {
        FixedCapacityRingBuffer<CountedElement> buffer(3);
        for (int value = 0; value < 50; ++value) {
            if (buffer.full()) {
                buffer.pop_front();
            }
            ASSERT_TRUE(buffer.emplace_back(value));
            ASSERT_EQ(CountedElement::live_count(), static_cast<int>(buffer.size()));
        }
        while (!buffer.empty()) {
            buffer.pop_front();
        }
        EXPECT_EQ(CountedElement::live_count(), 0);
    }
    EXPECT_EQ(CountedElement::live_count(), 0);
    EXPECT_EQ(CountedElement::constructions, 50);
}

TEST_F(FixedCapacityRingBufferTest, ElementsStillHeldAreDestroyedWithTheBuffer) {
    {
        FixedCapacityRingBuffer<CountedElement> buffer(4);
        // Wrap the positions first, so the elements left behind straddle the end of the storage.
        for (int value = 0; value < 3; ++value) {
            ASSERT_TRUE(buffer.emplace_back(value));
        }
        buffer.pop_front();
        buffer.pop_front();
        for (int value = 3; value < 6; ++value) {
            ASSERT_TRUE(buffer.emplace_back(value));
        }
        ASSERT_EQ(buffer.size(), 4u);
        ASSERT_EQ(CountedElement::live_count(), 4);
    }
    EXPECT_EQ(CountedElement::live_count(), 0);
    EXPECT_EQ(CountedElement::destructions, CountedElement::constructions);
}

TEST_F(FixedCapacityRingBufferTest, AConstructorThatThrowsLeavesTheBufferUnchanged) {
    FixedCapacityRingBuffer<ThrowingElement> buffer(2);
    ASSERT_TRUE(buffer.emplace_back(1));
    EXPECT_THROW((void)buffer.emplace_back(-1), std::runtime_error);
    EXPECT_EQ(buffer.size(), 1u);
    EXPECT_EQ(buffer.front().payload, 1);
    ASSERT_TRUE(buffer.emplace_back(2));
    EXPECT_TRUE(buffer.full());
}

TEST_F(FixedCapacityRingBufferTest, AnOverAlignedElementTypeIsStoredCorrectlyAligned) {
    FixedCapacityRingBuffer<OverAlignedElement> buffer(5);
    for (int value = 0; value < 23; ++value) {
        if (buffer.full()) {
            buffer.pop_front();
        }
        ASSERT_TRUE(buffer.emplace_back(value));
        const auto address = reinterpret_cast<std::uintptr_t>(&buffer.front());
        ASSERT_EQ(address % alignof(OverAlignedElement), 0u) << "after " << value << " additions";
    }
}

TEST_F(FixedCapacityRingBufferTest, TheStorageIsReportedOnceWhenTheBufferIsCreated) {
    AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 0;
    std::vector<size_t> reported_sizes;
    reporter.on_large_allocation = [&reported_sizes](size_t bytes, size_t) { reported_sizes.push_back(bytes); };

    FixedCapacityRingBuffer<int64_t> buffer(1000, &reporter);
    ASSERT_EQ(reported_sizes.size(), 1u);
    EXPECT_EQ(reported_sizes[0], 1000u * sizeof(int64_t));
    EXPECT_EQ(reporter.largest_allocation_bytes.load(), 1000u * sizeof(int64_t));

    // Adding and removing elements, full and refusing, and wrapping: none of it is reported,
    // because none of it allocates.
    for (int64_t value = 0; value < 5000; ++value) {
        if (buffer.full()) {
            EXPECT_FALSE(buffer.push_back(value));
            buffer.pop_front();
        }
        ASSERT_TRUE(buffer.push_back(value));
    }
    EXPECT_EQ(reported_sizes.size(), 1u);
}

TEST_F(FixedCapacityRingBufferTest, StorageBelowTheReportersThresholdIsNotReported) {
    AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1024;
    int reports = 0;
    reporter.on_large_allocation = [&reports](size_t, size_t) { ++reports; };

    const FixedCapacityRingBuffer<int64_t> small_buffer(100, &reporter);
    EXPECT_EQ(reports, 0);
    const FixedCapacityRingBuffer<int64_t> large_buffer(200, &reporter);
    EXPECT_EQ(reports, 1);
}

TEST_F(FixedCapacityRingBufferTest, AskingForTheFrontOfAnEmptyBufferIsRefused) {
    FixedCapacityRingBuffer<int> buffer(2);
    EXPECT_THROW((void)buffer.front(), PreconditionAssertion);
    const FixedCapacityRingBuffer<int>& const_buffer = buffer;
    EXPECT_THROW((void)const_buffer.front(), PreconditionAssertion);

    ASSERT_TRUE(buffer.push_back(1));
    buffer.pop_front();
    EXPECT_THROW((void)buffer.front(), PreconditionAssertion);
}

TEST_F(FixedCapacityRingBufferTest, RemovingTheFrontOfAnEmptyBufferIsRefused) {
    FixedCapacityRingBuffer<int> buffer(2);
    EXPECT_THROW(buffer.pop_front(), PreconditionAssertion);
    EXPECT_EQ(buffer.size(), 0u);

    ASSERT_TRUE(buffer.push_back(1));
    buffer.pop_front();
    EXPECT_THROW(buffer.pop_front(), PreconditionAssertion);
}

TEST_F(FixedCapacityRingBufferTest, MovingPassesTheElementsAndTheStorageToTheNewOwner) {
    FixedCapacityRingBuffer<CountedElement> source(3);
    ASSERT_TRUE(source.emplace_back(1));
    ASSERT_TRUE(source.emplace_back(2));
    const int constructions_before_move = CountedElement::constructions;

    FixedCapacityRingBuffer<CountedElement> destination(std::move(source));
    EXPECT_EQ(CountedElement::constructions, constructions_before_move) << "moving the buffer moved its elements one by one";
    EXPECT_EQ(destination.capacity(), 3u);
    EXPECT_EQ(destination.size(), 2u);
    EXPECT_EQ(destination.front().payload, 1);
    // The buffer moved from holds nothing, and refuses an element, having no storage to put it in.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_EQ(source.capacity(), 0u);
    EXPECT_TRUE(source.empty());
    EXPECT_FALSE(source.emplace_back(3));
}

TEST_F(FixedCapacityRingBufferTest, MoveAssignmentDestroysWhatTheTargetHeld) {
    {
        FixedCapacityRingBuffer<CountedElement> target(2);
        ASSERT_TRUE(target.emplace_back(10));
        ASSERT_TRUE(target.emplace_back(11));
        FixedCapacityRingBuffer<CountedElement> source(4);
        ASSERT_TRUE(source.emplace_back(20));
        ASSERT_EQ(CountedElement::live_count(), 3);

        move_assign(target, source);
        EXPECT_EQ(CountedElement::live_count(), 1);
        EXPECT_EQ(target.capacity(), 4u);
        EXPECT_EQ(target.front().payload, 20);

        move_assign(target, target);
        EXPECT_EQ(target.size(), 1u) << "a buffer assigned to itself lost its elements";
        EXPECT_EQ(target.front().payload, 20);
    }
    EXPECT_EQ(CountedElement::live_count(), 0);
}

TEST_F(FixedCapacityRingBufferTest, HoldsAnElementTypeThatOwnsHeapMemory) {
    FixedCapacityRingBuffer<std::unique_ptr<int>> buffer(2);
    ASSERT_TRUE(buffer.push_back(std::make_unique<int>(1)));
    ASSERT_TRUE(buffer.push_back(std::make_unique<int>(2)));
    buffer.pop_front();
    ASSERT_TRUE(buffer.push_back(std::make_unique<int>(3)));
    EXPECT_EQ(*buffer.front(), 2);
}

} // namespaces
