// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/LockedMemory.hpp>
#include <pubsub_itc_fw/MemoryLockingAllocator.hpp>
#include <pubsub_itc_fw/tests_common/LockedMemoryForTests.hpp>

namespace pubsub_itc_fw::tests {

namespace {

using tests_common::locked_kilobytes;
using tests_common::LockedMemoryLimitLowered;

// Large enough that the kernel's count of locked memory, in kB, moves by a clear amount.
constexpr size_t one_mebibyte = 1024UL * 1024UL;

} // un-named namespace

TEST(MemoryLockingAllocatorTest, WhatItAllocatesIsLockedAndCountedAndFreeingItUnlocksIt) {
    MemoryLockRecord record;
    const size_t before = locked_kilobytes();
    {
        std::vector<uint8_t, MemoryLockingAllocator<uint8_t>> block(MemoryLockingAllocator<uint8_t>(nullptr, &record));
        block.resize(one_mebibyte);
        EXPECT_EQ(record.error, 0);
        EXPECT_EQ(record.locked_bytes, one_mebibyte);
        EXPECT_GE(locked_kilobytes() - before, one_mebibyte / 1024) << "the kernel must count the block as locked";
    }
    EXPECT_EQ(record.locked_bytes, 0U);
    EXPECT_EQ(locked_kilobytes(), before) << "freeing the block must unlock it";
}

TEST(MemoryLockingAllocatorTest, WithNoRecordNothingIsLocked) {
    const size_t before = locked_kilobytes();
    std::vector<uint8_t, MemoryLockingAllocator<uint8_t>> block(one_mebibyte, 0, MemoryLockingAllocator<uint8_t>());
    EXPECT_EQ(locked_kilobytes(), before);
}

TEST(MemoryLockingAllocatorTest, ABlockThatCannotBeLockedIsStillGivenAndTheFailureIsRecorded) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may lock memory whatever its limit, so a lowered limit cannot make locking fail";
    }
    MemoryLockRecord record;
    const LockedMemoryLimitLowered limit(64 * 1024);
    std::vector<uint8_t, MemoryLockingAllocator<uint8_t>> block(MemoryLockingAllocator<uint8_t>(nullptr, &record));
    block.resize(one_mebibyte, 7);
    EXPECT_EQ(record.error, ENOMEM);
    EXPECT_EQ(record.locked_bytes, 0U);
    EXPECT_EQ(block[one_mebibyte - 1], 7) << "the block must be usable although it is not locked";
    EXPECT_NE(why_locking_failed(record.error).find("ENOMEM"), std::string::npos);
}

TEST(MemoryLockingAllocatorTest, OnlyTheFirstFailureIsKept) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may lock memory whatever its limit, so a lowered limit cannot make locking fail";
    }
    MemoryLockRecord record;
    record.error = EAGAIN;
    const LockedMemoryLimitLowered limit(64 * 1024);
    std::vector<uint8_t, MemoryLockingAllocator<uint8_t>> block(one_mebibyte, 0, MemoryLockingAllocator<uint8_t>(nullptr, &record));
    EXPECT_EQ(record.error, EAGAIN);
}

TEST(MemoryLockingAllocatorTest, AHashMapLocksItsTableThroughTheRebindAndStillReportsItsGrowth) {
    using Allocator = MemoryLockingAllocator<std::pair<uint64_t, uint64_t>>;
    MemoryLockRecord record;
    AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1;
    size_t reported = 0;
    reporter.on_large_allocation = [&reported](size_t bytes, size_t /*largest*/) { reported = bytes; };
    const size_t before = locked_kilobytes();
    {
        tsl::robin_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, Allocator> map(0, std::hash<uint64_t>{}, std::equal_to<uint64_t>{},
                                                                                                        Allocator(&reporter, &record));
        map.reserve(100'000);
        EXPECT_EQ(record.error, 0);
        EXPECT_GT(record.locked_bytes, 0U);
        EXPECT_EQ(record.locked_bytes, reported) << "the one table allocated is the one locked";
        EXPECT_GE(locked_kilobytes() - before, record.locked_bytes / 1024);
    }
    EXPECT_EQ(locked_kilobytes(), before);
}

TEST(MemoryLockingAllocatorTest, TwoAllocatorsAreEqualOnlyWhenTheyReportAndRecordToTheSamePlaces) {
    MemoryLockRecord record;
    MemoryLockRecord other_record;
    AllocationGrowthReporter reporter;
    const MemoryLockingAllocator<int> first(&reporter, &record);
    EXPECT_TRUE(first == MemoryLockingAllocator<int>(&reporter, &record));
    EXPECT_TRUE(first != MemoryLockingAllocator<int>(&reporter, &other_record));
    EXPECT_TRUE(first != MemoryLockingAllocator<int>(nullptr, &record));
    const MemoryLockingAllocator<double> rebound(first);
    EXPECT_EQ(rebound.lock_record(), &record);
    EXPECT_EQ(rebound.reporter(), &reporter);
}

TEST(LockedMemoryLimitTest, ALimitBelowWhatIsWantedDoesNotAllowItAndSaysWhatItIs) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may lock memory whatever its limit";
    }
    const LockedMemoryLimitLowered limit(4 * one_mebibyte);
    const LockedMemoryLimit read = LockedMemoryLimit::current();
    EXPECT_TRUE(read.allows(4 * one_mebibyte));
    EXPECT_FALSE(read.allows((4 * one_mebibyte) + 1));
    EXPECT_EQ(read.text(), "4 MB");
}

} // namespaces
