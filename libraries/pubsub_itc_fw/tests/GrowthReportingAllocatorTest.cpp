// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/GrowthReportingAllocator.hpp>

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t small_threshold_bytes = 64;

} // namespaces

class GrowthReportingAllocatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        reporter_.report_threshold_bytes = small_threshold_bytes;
        reporter_.on_large_allocation = [this](size_t bytes, size_t largest) {
            ++reports_;
            last_reported_bytes_ = bytes;
            last_reported_largest_ = largest;
        };
    }

    AllocationGrowthReporter reporter_;
    size_t reports_{0};
    size_t last_reported_bytes_{0};
    size_t last_reported_largest_{0};
};

TEST_F(GrowthReportingAllocatorTest, AllocationAboveTheThresholdIsReported) {
    GrowthReportingAllocator<int> allocator(&reporter_);
    int* block = allocator.allocate(small_threshold_bytes);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reports_, 1u);
    EXPECT_EQ(last_reported_bytes_, small_threshold_bytes * sizeof(int));
    allocator.deallocate(block, small_threshold_bytes);
}

TEST_F(GrowthReportingAllocatorTest, AllocationBelowTheThresholdIsNotReported) {
    GrowthReportingAllocator<int> allocator(&reporter_);
    int* block = allocator.allocate(1);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reports_, 0u);
    allocator.deallocate(block, 1);
}

TEST_F(GrowthReportingAllocatorTest, TheLargestAllocationIsRemembered) {
    GrowthReportingAllocator<int> allocator(&reporter_);
    int* small_block = allocator.allocate(small_threshold_bytes);
    int* large_block = allocator.allocate(small_threshold_bytes * 4);
    int* medium_block = allocator.allocate(small_threshold_bytes * 2);

    EXPECT_EQ(reports_, 3u);
    // The high-water mark stands: the last allocation was smaller and must not lower it.
    EXPECT_EQ(reporter_.largest_allocation_bytes.load(), small_threshold_bytes * 4 * sizeof(int));
    EXPECT_EQ(last_reported_largest_, small_threshold_bytes * 4 * sizeof(int));

    allocator.deallocate(small_block, small_threshold_bytes);
    allocator.deallocate(large_block, small_threshold_bytes * 4);
    allocator.deallocate(medium_block, small_threshold_bytes * 2);
}

TEST_F(GrowthReportingAllocatorTest, AnAllocatorWithNoReporterAllocatesSilently) {
    GrowthReportingAllocator<int> allocator;
    int* block = allocator.allocate(small_threshold_bytes * 8);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(reports_, 0u);
    allocator.deallocate(block, small_threshold_bytes * 8);
}

TEST_F(GrowthReportingAllocatorTest, AllocatorTraitsRebindsWithoutANestedRebindStruct) {
    // The nested rebind member was removed as the C++98 spelling of this. allocator_traits
    // synthesises the rebound type by substituting the first template argument, and this test
    // is what says so: without it, the removal would only be discovered by a container.
    using SourceAllocator = GrowthReportingAllocator<int>;
    using ReboundAllocator = std::allocator_traits<SourceAllocator>::rebind_alloc<std::string>;
    static_assert(std::is_same<ReboundAllocator, GrowthReportingAllocator<std::string>>::value,
                  "allocator_traits must rebind GrowthReportingAllocator by substituting its value type");

    SourceAllocator source(&reporter_);
    ReboundAllocator rebound(source);
    EXPECT_EQ(rebound.reporter(), &reporter_) << "the reporter was lost when the allocator was rebound";
}

TEST_F(GrowthReportingAllocatorTest, TwoAllocatorsAreEqualWhenTheyReportToTheSamePlace) {
    GrowthReportingAllocator<int> first(&reporter_);
    GrowthReportingAllocator<int> second(&reporter_);
    AllocationGrowthReporter other_reporter;
    GrowthReportingAllocator<int> third(&other_reporter);

    EXPECT_TRUE(first == second);
    EXPECT_FALSE(first != second);
    EXPECT_TRUE(first != third);

    // Equality survives a rebind, which is what lets a container compare the allocator it was
    // given against the one its internal storage holds.
    GrowthReportingAllocator<std::string> rebound(first);
    EXPECT_TRUE(first == rebound);
}

TEST_F(GrowthReportingAllocatorTest, AVectorReportsThroughIt) {
    std::vector<int, GrowthReportingAllocator<int>> values{GrowthReportingAllocator<int>(&reporter_)};
    for (int value = 0; value < 1000; ++value) {
        values.push_back(value);
    }
    EXPECT_GT(reports_, 0u);
    EXPECT_EQ(values.size(), 1000u);
    EXPECT_GE(reporter_.largest_allocation_bytes.load(), values.size() * sizeof(int));
}

TEST_F(GrowthReportingAllocatorTest, AHashMapReportsThroughItAfterTheRebind) {
    // A container that rebinds for real, rather than the static_assert above: a hash map never
    // allocates the type it was given, only its own node or bucket type, so this is the path
    // every documented use of this allocator actually takes. tsl::robin_map -- the use the
    // header's example shows -- rebinds through allocator_traits in exactly the same way, and
    // is not used here only because the framework tests do not link a third-party map.
    using MapAllocator = GrowthReportingAllocator<std::pair<const int, int>>;
    std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, MapAllocator> map(0, std::hash<int>{}, std::equal_to<int>{}, MapAllocator{&reporter_});

    for (int key = 0; key < 5000; ++key) {
        map.emplace(key, key);
    }
    EXPECT_EQ(map.size(), 5000u);
    EXPECT_GT(reports_, 0u) << "the reporter was lost on the way through the map's rebind";
    EXPECT_GT(reporter_.largest_allocation_bytes.load(), small_threshold_bytes);
}

} // namespaces
