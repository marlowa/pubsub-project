// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <map>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/SendsToGoneConnections.hpp>

using pubsub_itc_fw::SendsToGoneConnections;

namespace {

std::map<int, int64_t> report_of(SendsToGoneConnections& sends) {
    std::map<int, int64_t> reported;
    sends.report_and_forget([&reported](int connection_id, int64_t count) {
        EXPECT_EQ(reported.count(connection_id), 0U) << "a connection was reported twice";
        reported[connection_id] = count;
    });
    return reported;
}

} // namespaces

TEST(SendsToGoneConnectionsTest, ManySendsToEachConnectionAreOneReportForEachWithTheCount) {
    SendsToGoneConnections sends;
    for (int send = 0; send < 1000; ++send) {
        sends.count_send(7);
    }
    sends.count_send(9);
    EXPECT_EQ(report_of(sends), (std::map<int, int64_t>{{7, 1000}, {9, 1}}));
}

TEST(SendsToGoneConnectionsTest, AReportIsNotRepeatedAndCountingStartsAgainAfterIt) {
    SendsToGoneConnections sends;
    sends.count_send(7);
    EXPECT_EQ(report_of(sends).size(), 1U);
    EXPECT_TRUE(report_of(sends).empty());
    sends.count_send(7);
    EXPECT_EQ(report_of(sends), (std::map<int, int64_t>{{7, 1}}));
}
