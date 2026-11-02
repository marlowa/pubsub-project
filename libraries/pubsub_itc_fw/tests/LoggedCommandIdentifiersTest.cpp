// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiers.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

using pubsub_itc_fw::LoggedCommandIdentifiers;
using pubsub_itc_fw::WalPosition;

TEST(LoggedCommandIdentifiersTest, ACommandAddedIsPossiblyHeldAtItsPositionAndOneNotAddedIsNot) {
    LoggedCommandIdentifiers record(100, nullptr);
    const uint64_t added = LoggedCommandIdentifiers::identifier("MEMBER1/order-1");
    const uint64_t other = LoggedCommandIdentifiers::identifier("MEMBER1/order-2");
    EXPECT_EQ(record.add(added, WalPosition{3, 4096}), LoggedCommandIdentifiers::Added::added);
    const LoggedCommandIdentifiers::Lookup found = record.find(added);
    EXPECT_TRUE(found.may_hold);
    EXPECT_TRUE(found.position_known);
    EXPECT_EQ(found.position.segment, 3U);
    EXPECT_EQ(found.position.offset, 4096U);
    EXPECT_FALSE(record.find(other).may_hold);
    EXPECT_EQ(record.size(), 1U);
}

// A later command with the same number does not move the position: the first is the one the WAL reads, and a
// search finds the others.
TEST(LoggedCommandIdentifiersTest, TheFirstPositionGivenForANumberIsKept) {
    LoggedCommandIdentifiers record(100, nullptr);
    const uint64_t id = LoggedCommandIdentifiers::identifier("MEMBER1/order-1");
    EXPECT_EQ(record.add(id, WalPosition{1, 100}), LoggedCommandIdentifiers::Added::added);
    EXPECT_EQ(record.add(id, WalPosition{2, 200}), LoggedCommandIdentifiers::Added::already_present);
    EXPECT_EQ(record.find(id).position.segment, 1U);
    EXPECT_EQ(record.find(id).position.offset, 100U);
}

TEST(LoggedCommandIdentifiersTest, APositionTooLargeToPackIsHeldAsUnknown) {
    LoggedCommandIdentifiers record(100, nullptr);
    const uint64_t id = LoggedCommandIdentifiers::identifier("MEMBER1/order-1");
    EXPECT_EQ(record.add(id, WalPosition{uint64_t{1} << 16U, 0}), LoggedCommandIdentifiers::Added::added);
    EXPECT_TRUE(record.find(id).may_hold);
    EXPECT_FALSE(record.find(id).position_known);
}

TEST(LoggedCommandIdentifiersTest, APositionPacksAndUnpacksUnchanged) {
    const WalPosition position{65535, (uint64_t{1} << 48U) - 1};
    const WalPosition back = LoggedCommandIdentifiers::unpack(LoggedCommandIdentifiers::pack(position));
    EXPECT_EQ(back.segment, position.segment);
    EXPECT_EQ(back.offset, position.offset);
    EXPECT_EQ(LoggedCommandIdentifiers::pack(WalPosition{0, uint64_t{1} << 48U}), LoggedCommandIdentifiers::no_position);
}

TEST(LoggedCommandIdentifiersTest, AMillionDistinctIdentitiesGiveAMillionDistinctNumbers) {
    std::set<uint64_t> seen;
    for (int number = 0; number < 1000000; ++number) {
        seen.insert(LoggedCommandIdentifiers::identifier("MEMBER" + std::to_string(number % 1000) + "/order-" + std::to_string(number)));
    }
    EXPECT_EQ(seen.size(), 1000000U);
}

// The table is reserved once and must never grow: a rehash of a table this size would stall the thread that
// sequences orders. Reserving 900,000 at a maximum fill of 0.9 needs at least 1,000,000 slots, which rounds up
// to 2^20.
TEST(LoggedCommandIdentifiersTest, TheTableIsReservedOnceAndNeverGrowsUpToItsCapacity) {
    const size_t capacity = 900000;
    LoggedCommandIdentifiers record(capacity, nullptr);
    EXPECT_EQ(record.slot_count(), size_t{1} << 20U);
    for (size_t number = 0; number < capacity; ++number) {
        ASSERT_EQ(record.add(LoggedCommandIdentifiers::identifier(std::to_string(number)), WalPosition{0, number}), LoggedCommandIdentifiers::Added::added);
    }
    EXPECT_EQ(record.slot_count(), size_t{1} << 20U);
    EXPECT_TRUE(record.full());
}

TEST(LoggedCommandIdentifiersTest, AFullRecordRefusesANewIdentityButStillRecognisesAnOldOne) {
    LoggedCommandIdentifiers record(2, nullptr);
    const uint64_t first = LoggedCommandIdentifiers::identifier("1");
    const uint64_t second = LoggedCommandIdentifiers::identifier("2");
    const uint64_t third = LoggedCommandIdentifiers::identifier("3");
    EXPECT_EQ(record.add(first, WalPosition{0, 0}), LoggedCommandIdentifiers::Added::added);
    EXPECT_EQ(record.add(second, WalPosition{0, 64}), LoggedCommandIdentifiers::Added::added);
    EXPECT_EQ(record.add(third, WalPosition{0, 128}), LoggedCommandIdentifiers::Added::full);
    EXPECT_FALSE(record.find(third).may_hold);
    EXPECT_EQ(record.add(first, WalPosition{0, 0}), LoggedCommandIdentifiers::Added::already_present);
    EXPECT_EQ(record.size(), 2U);
}

// A slot holds the number, the packed position and the table's own bookkeeping: 24 bytes.
TEST(LoggedCommandIdentifiersTest, TheTableIsReportedToTheGrowthReporter) {
    pubsub_itc_fw::AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1024;
    size_t reported = 0;
    reporter.on_large_allocation = [&reported](size_t bytes, size_t /*largest*/) { reported = bytes; };
    const LoggedCommandIdentifiers record(900000, &reporter);
    EXPECT_EQ(reported, (size_t{1} << 20U) * 24U);
    EXPECT_EQ(record.capacity(), 900000U);
}

TEST(LoggedCommandIdentifiersTest, ACapacityOfZeroIsRefused) {
    EXPECT_THROW(LoggedCommandIdentifiers(0, nullptr), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LoggedCommandIdentifiersTest, TheNumberFunctionGivenIsTheOneUsed) {
    const LoggedCommandIdentifiers record(10, nullptr, [](std::string_view /*identity*/) -> uint64_t { return 7; });
    EXPECT_EQ(record.number_of("anything"), 7U);
}
