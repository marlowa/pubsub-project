// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file SessionCancelsPendingTest.cpp
 * @brief Unit tests for sequencer::SessionCancelsPending: the cancels on disconnect the venue is waiting to carry out.
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "SessionCancelsPending.hpp"
#include "SessionIdentity.hpp"

namespace sequencer::tests {

namespace {

constexpr int64_t second = 1'000'000'000;
constexpr int64_t ended_at = 1'000 * second;

const fix_common::SessionIdentity member = fix_common::SessionIdentity::make("MEMBER", gateway_ids::fix_order_gateway);
const fix_common::SessionIdentity other = fix_common::SessionIdentity::make("OTHER", gateway_ids::fix_order_gateway);

std::vector<fix_common::SessionIdentity> due_at(const SessionCancelsPending& pending, int64_t now_ns) {
    std::vector<fix_common::SessionIdentity> due;
    pending.for_each_due(now_ns, [&due](const fix_common::SessionIdentity& identity) { due.push_back(identity); });
    return due;
}

} // namespaces

// A wait is due once its grace period has run from the time its end was recorded, and not a moment before.
TEST(SessionCancelsPendingTest, AWaitIsDueWhenItsGracePeriodHasRunAndNotBefore) {
    SessionCancelsPending pending;
    pending.ended(member, 10, ended_at, 8);
    EXPECT_TRUE(pending.pending(member));
    EXPECT_TRUE(due_at(pending, ended_at + 8 * second - 1).empty());
    const auto due = due_at(pending, ended_at + 8 * second);
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0], member);
}

// A grace period of zero, as after a clean Logout, is due at once.
TEST(SessionCancelsPendingTest, AGracePeriodOfZeroIsDueAtOnce) {
    SessionCancelsPending pending;
    pending.ended(member, 10, ended_at, 0);
    EXPECT_EQ(due_at(pending, ended_at).size(), 1u);
}

// A session that binds again in time is no longer waiting, and one whose cancel was written is not either.
TEST(SessionCancelsPendingTest, AWaitEndsWhenWithdrawnOrCarriedOut) {
    SessionCancelsPending pending;
    pending.ended(member, 10, ended_at, 8);
    pending.ended(other, 11, ended_at, 8);
    pending.withdrawn(member);
    pending.carried_out(other);
    EXPECT_FALSE(pending.pending(member));
    EXPECT_FALSE(pending.pending(other));
    EXPECT_TRUE(due_at(pending, ended_at + 60 * second).empty());
}

// A second end recorded for a session already waiting does not move its deadline: the member's grace period runs from
// the first.
TEST(SessionCancelsPendingTest, ASecondEndDoesNotMoveTheDeadline) {
    SessionCancelsPending pending;
    pending.ended(member, 10, ended_at, 8);
    pending.ended(member, 12, ended_at + 5 * second, 8);
    EXPECT_EQ(due_at(pending, ended_at + 8 * second).size(), 1u);
}

// Records discarded from the WAL take the waits they started with them, and leave the others.
TEST(SessionCancelsPendingTest, DiscardingRecordsForgetsTheWaitsTheyStarted) {
    SessionCancelsPending pending;
    pending.ended(member, 10, ended_at, 8);
    pending.ended(other, 20, ended_at, 8);
    pending.discard_after(15);
    EXPECT_TRUE(pending.pending(member));
    EXPECT_FALSE(pending.pending(other));
}

} // namespaces
