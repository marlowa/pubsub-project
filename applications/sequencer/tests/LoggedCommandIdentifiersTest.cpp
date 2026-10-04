// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "LoggedCommandIdentifiers.hpp"

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

using sequencer::LoggedCommandIdentifiers;

TEST(LoggedCommandIdentifiersTest, ACommandAddedIsPossiblyHeldAndOneNotAddedIsNot) {
    LoggedCommandIdentifiers record(100, nullptr);
    const uint64_t added = LoggedCommandIdentifiers::identifier("MEMBER1", 1, "order-1");
    const uint64_t other = LoggedCommandIdentifiers::identifier("MEMBER1", 1, "order-2");
    EXPECT_EQ(record.add(added), LoggedCommandIdentifiers::Added::added);
    EXPECT_TRUE(record.may_hold(added));
    EXPECT_FALSE(record.may_hold(other));
    EXPECT_EQ(record.add(added), LoggedCommandIdentifiers::Added::already_present);
    EXPECT_EQ(record.size(), 1U);
}

TEST(LoggedCommandIdentifiersTest, EachOfTheThreeValuesChangesTheIdentifier) {
    const uint64_t base = LoggedCommandIdentifiers::identifier("MEMBER1", 1, "order-1");
    EXPECT_NE(base, LoggedCommandIdentifiers::identifier("MEMBER2", 1, "order-1"));
    EXPECT_NE(base, LoggedCommandIdentifiers::identifier("MEMBER1", 2, "order-1"));
    EXPECT_NE(base, LoggedCommandIdentifiers::identifier("MEMBER1", 1, "order-2"));
    // The comp id and the ClOrdID are not simply joined: moving characters from one to the other
    // gives a different identifier.
    EXPECT_NE(LoggedCommandIdentifiers::identifier("AB", 1, "C"), LoggedCommandIdentifiers::identifier("A", 1, "BC"));
    EXPECT_EQ(base, LoggedCommandIdentifiers::identifier("MEMBER1", 1, "order-1"));
}

TEST(LoggedCommandIdentifiersTest, AMillionDistinctCommandsGiveAMillionDistinctIdentifiers) {
    std::set<uint64_t> seen;
    for (int number = 0; number < 1000000; ++number) {
        seen.insert(LoggedCommandIdentifiers::identifier("MEMBER" + std::to_string(number % 1000), 1, "order-" + std::to_string(number)));
    }
    EXPECT_EQ(seen.size(), 1000000U);
}

// The table is reserved once and must never grow: a rehash of a table this size would stall the
// thread that sequences orders. Reserving 900,000 at a maximum fill of 0.9 needs at least 1,000,000
// slots, which rounds up to 2^20.
TEST(LoggedCommandIdentifiersTest, TheTableIsReservedOnceAndNeverGrowsUpToItsCapacity) {
    const size_t capacity = 900000;
    LoggedCommandIdentifiers record(capacity, nullptr);
    EXPECT_EQ(record.slot_count(), size_t{1} << 20U);
    for (size_t number = 0; number < capacity; ++number) {
        ASSERT_EQ(record.add(LoggedCommandIdentifiers::identifier("M", 1, std::to_string(number))), LoggedCommandIdentifiers::Added::added);
    }
    EXPECT_EQ(record.slot_count(), size_t{1} << 20U);
    EXPECT_TRUE(record.full());
}

TEST(LoggedCommandIdentifiersTest, AFullRecordRefusesANewIdentifierButStillRecognisesAnOldOne) {
    LoggedCommandIdentifiers record(2, nullptr);
    const uint64_t first = LoggedCommandIdentifiers::identifier("M", 1, "1");
    const uint64_t second = LoggedCommandIdentifiers::identifier("M", 1, "2");
    const uint64_t third = LoggedCommandIdentifiers::identifier("M", 1, "3");
    EXPECT_EQ(record.add(first), LoggedCommandIdentifiers::Added::added);
    EXPECT_EQ(record.add(second), LoggedCommandIdentifiers::Added::added);
    EXPECT_EQ(record.add(third), LoggedCommandIdentifiers::Added::full);
    EXPECT_FALSE(record.may_hold(third));
    EXPECT_EQ(record.add(first), LoggedCommandIdentifiers::Added::already_present);
    EXPECT_EQ(record.size(), 2U);
}

TEST(LoggedCommandIdentifiersTest, TheTableIsReportedToTheGrowthReporter) {
    pubsub_itc_fw::AllocationGrowthReporter reporter;
    reporter.report_threshold_bytes = 1024;
    size_t reported = 0;
    reporter.on_large_allocation = [&reported](size_t bytes, size_t /*largest*/) { reported = bytes; };
    const LoggedCommandIdentifiers record(900000, &reporter);
    // 2^20 slots of 16 bytes.
    EXPECT_EQ(reported, (size_t{1} << 20U) * 16U);
    EXPECT_EQ(record.capacity(), 900000U);
}

TEST(LoggedCommandIdentifiersTest, ACapacityOfZeroIsRefused) {
    EXPECT_THROW(LoggedCommandIdentifiers(0, nullptr), pubsub_itc_fw::PreconditionAssertion);
}
