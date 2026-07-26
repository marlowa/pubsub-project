// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <CancelClOrdId.hpp>

#include <array>
#include <set>
#include <string>

#include <gtest/gtest.h>

// Both gateways generate a ClOrdID for every cancel they send on a dead client's behalf, on
// a path that can produce thousands in a burst. These tests pin what that id must be: unique
// per cancel, bounded by the shared ClOrdID limit, and produced without allocating.

namespace {

TEST(CancelClOrdIdTest, FormatsPrefixSessionAndCounter) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};
    const std::string_view formatted = cancel_cl_ord_id::format(buffer, "BGW-CXL-", 42, 7);

    EXPECT_EQ(formatted, "BGW-CXL-42-7");
}

TEST(CancelClOrdIdTest, TheViewPointsIntoTheCallersBuffer) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};
    const std::string_view formatted = cancel_cl_ord_id::format(buffer, "GW-CXL-", 1, 1);

    EXPECT_EQ(formatted.data(), buffer.data()) << "the view must not own storage of its own";
}

// The id has to be unique per cancel or a client's cancels would collide with each other in
// the matching engine's book.
TEST(CancelClOrdIdTest, EveryCounterValueGivesADistinctId) {
    std::set<std::string> seen;
    for (int counter = 1; counter <= 1000; ++counter) {
        std::array<char, cancel_cl_ord_id::max_length> buffer{};
        seen.insert(std::string(cancel_cl_ord_id::format(buffer, "BGW-CXL-", 5, counter)));
    }
    EXPECT_EQ(seen.size(), 1000U);
}

TEST(CancelClOrdIdTest, DifferentSessionsGiveDistinctIds) {
    std::array<char, cancel_cl_ord_id::max_length> first_buffer{};
    std::array<char, cancel_cl_ord_id::max_length> second_buffer{};

    const std::string first(cancel_cl_ord_id::format(first_buffer, "BGW-CXL-", 1, 1));
    const std::string second(cancel_cl_ord_id::format(second_buffer, "BGW-CXL-", 2, 1));

    EXPECT_NE(first, second);
}

TEST(CancelClOrdIdTest, HandlesTheExtremesOfTheSessionIdRange) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};

    EXPECT_EQ(cancel_cl_ord_id::format(buffer, "BGW-CXL-", 2147483647, 999999), "BGW-CXL-2147483647-999999");
    EXPECT_EQ(cancel_cl_ord_id::format(buffer, "BGW-CXL-", -1, 1), "BGW-CXL--1-1");
}

// A generated id must never exceed what the book key holds, or two cancels could truncate to
// one. Reporting failure is right here: the caller logs and leaves the order alone rather
// than sending a cancel that might retire something else.
TEST(CancelClOrdIdTest, ReturnsEmptyRatherThanTruncateWhenThePrefixFillsTheBuffer) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};
    const std::string over_long_prefix(cancel_cl_ord_id::max_length + 1, 'X');

    EXPECT_TRUE(cancel_cl_ord_id::format(buffer, over_long_prefix, 1, 1).empty());
}

TEST(CancelClOrdIdTest, ReturnsEmptyWhenTheDigitsWouldNotFit) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};
    // A prefix leaving room for only a few characters cannot hold two integers and a dash.
    const std::string prefix(cancel_cl_ord_id::max_length - 3, 'X');

    EXPECT_TRUE(cancel_cl_ord_id::format(buffer, prefix, 2147483647, 2147483647).empty());
}

TEST(CancelClOrdIdTest, NeverExceedsTheSharedClOrdIdMaximum) {
    std::array<char, cancel_cl_ord_id::max_length> buffer{};
    const std::string_view formatted = cancel_cl_ord_id::format(buffer, "BGW-CXL-", 2147483647, 2147483647);

    EXPECT_LE(formatted.size(), fix_order_limits::max_cl_ord_id_length);
}

} // namespaces
