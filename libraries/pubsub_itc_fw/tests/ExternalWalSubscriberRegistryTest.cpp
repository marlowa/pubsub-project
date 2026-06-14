// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>

namespace pubsub_itc_fw::tests {

/*
 * Unit tests for ExternalWalSubscriberRegistry.
 *
 * This class tracks one WAL cursor per connected external WAL subscriber
 * (e.g. MEP primary and MEP secondary). A cursor is the seq_no of the last
 * WAL record the subscriber has acknowledged; the publisher must not delete
 * any WAL segment that still contains records at or after any subscriber's
 * cursor.
 *
 * min_cursor() returns the minimum cursor across all registered subscribers,
 * which is the safe truncation boundary. When no subscribers are registered
 * it returns no_constraint, meaning truncation is not bounded by any external
 * subscriber.
 *
 * The registry also detects orphaned connections: if a new connection arrives
 * with a subscriber_id that is already registered (e.g. a reconnect before
 * the old TCP socket has been torn down), the old connection is displaced and
 * its ConnectionID is returned to the caller so it can be disconnected.
 * This prevents duplicate stream delivery to the same logical subscriber.
 */
class ExternalWalSubscriberRegistryTest : public ::testing::Test {
  protected:
    ExternalWalSubscriberRegistry registry_;
};

// ---------------------------------------------------------------------------
// Empty registry
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, EmptyRegistryReturnsNoConstraint) {
    EXPECT_EQ(registry_.min_cursor(), ExternalWalSubscriberRegistry::no_constraint);
}

TEST_F(ExternalWalSubscriberRegistryTest, EmptyRegistryHasZeroSubscribers) {
    EXPECT_EQ(registry_.subscriber_count(), 0u);
}

// ---------------------------------------------------------------------------
// Single subscriber
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, SingleSubscriberMinCursorIsThatCursor) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    EXPECT_EQ(registry_.min_cursor(), 50);
}

TEST_F(ExternalWalSubscriberRegistryTest, SingleSubscriberCountIsOne) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    EXPECT_EQ(registry_.subscriber_count(), 1u);
}

TEST_F(ExternalWalSubscriberRegistryTest, NewSubscriberIdReturnsInvalidOrphan) {
    const ConnectionID orphan = registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    EXPECT_FALSE(orphan.is_valid());
}

// ---------------------------------------------------------------------------
// Two subscribers
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, TwoSubscribersMinCursorIsLower) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_secondary", 80);
    EXPECT_EQ(registry_.min_cursor(), 50);
}

TEST_F(ExternalWalSubscriberRegistryTest, TwoSubscribersCountIsTwo) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_secondary", 80);
    EXPECT_EQ(registry_.subscriber_count(), 2u);
}

// ---------------------------------------------------------------------------
// Updating cursors
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, UpdatingMinimumSubscriberRecomputesMinimum) {
    // A=50, B=80 -> min=50. Advance A to 90 -> min is now B at 80.
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_secondary", 80);
    registry_.update_cursor(ConnectionID{1}, 90);
    EXPECT_EQ(registry_.min_cursor(), 80);
}

TEST_F(ExternalWalSubscriberRegistryTest, UpdateCursorReturnsTrueForKnownConnection) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    EXPECT_TRUE(registry_.update_cursor(ConnectionID{1}, 60));
}

TEST_F(ExternalWalSubscriberRegistryTest, UpdateCursorReturnsFalseForUnknownConnection) {
    EXPECT_FALSE(registry_.update_cursor(ConnectionID{99}, 60));
}

// ---------------------------------------------------------------------------
// Removing subscribers
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, RemovingMinimumSubscriberRecomputesMinimum) {
    // A=50 is the minimum. Remove A -> min is now B at 80.
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_secondary", 80);
    registry_.remove_subscriber(ConnectionID{1});
    EXPECT_EQ(registry_.min_cursor(), 80);
}

TEST_F(ExternalWalSubscriberRegistryTest, RemovingAllSubscribersReturnsNoConstraint) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_secondary", 80);
    registry_.remove_subscriber(ConnectionID{1});
    registry_.remove_subscriber(ConnectionID{2});
    EXPECT_EQ(registry_.min_cursor(), ExternalWalSubscriberRegistry::no_constraint);
}

TEST_F(ExternalWalSubscriberRegistryTest, RemovingAllSubscribersCountIsZero) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.remove_subscriber(ConnectionID{1});
    EXPECT_EQ(registry_.subscriber_count(), 0u);
}

TEST_F(ExternalWalSubscriberRegistryTest, RemovingUnknownConnectionIsNoop) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.remove_subscriber(ConnectionID{99});
    EXPECT_EQ(registry_.subscriber_count(), 1u);
    EXPECT_EQ(registry_.min_cursor(), 50);
}

// ---------------------------------------------------------------------------
// Three subscribers
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, ThreeSubscribersMinIsLowest) {
    registry_.register_subscriber(ConnectionID{1}, "sub_a", 100);
    registry_.register_subscriber(ConnectionID{2}, "sub_b", 200);
    registry_.register_subscriber(ConnectionID{3}, "sub_c", 150);
    EXPECT_EQ(registry_.min_cursor(), 100);
}

TEST_F(ExternalWalSubscriberRegistryTest, RemovingLowestOfThreeGivesNextLowest) {
    registry_.register_subscriber(ConnectionID{1}, "sub_a", 100);
    registry_.register_subscriber(ConnectionID{2}, "sub_b", 200);
    registry_.register_subscriber(ConnectionID{3}, "sub_c", 150);
    registry_.remove_subscriber(ConnectionID{1});
    EXPECT_EQ(registry_.min_cursor(), 150);
}

// ---------------------------------------------------------------------------
// Orphan detection: reconnect before old TCP socket is torn down
// ---------------------------------------------------------------------------

TEST_F(ExternalWalSubscriberRegistryTest, DuplicateSubscriberIdReturnsOrphanedConnectionId) {
    // Connection 1 registers as mep_primary. Before it is torn down, connection 2
    // arrives with the same subscriber_id. The registry must displace connection 1
    // and return it as the orphan so the caller can disconnect it.
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    const ConnectionID orphan = registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    EXPECT_TRUE(orphan.is_valid());
    EXPECT_EQ(orphan, ConnectionID{1});
}

TEST_F(ExternalWalSubscriberRegistryTest, AfterOrphanDisplacementCountRemainsOne) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    EXPECT_EQ(registry_.subscriber_count(), 1u);
}

TEST_F(ExternalWalSubscriberRegistryTest, AfterOrphanDisplacementMinCursorIsNewCursor) {
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    EXPECT_EQ(registry_.min_cursor(), 60);
}

TEST_F(ExternalWalSubscriberRegistryTest, OrphanedConnectionNoLongerTracked) {
    // After displacement, update_cursor on the orphaned connection must return false
    // (it is no longer in the registry).
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    EXPECT_FALSE(registry_.update_cursor(ConnectionID{1}, 70));
}

TEST_F(ExternalWalSubscriberRegistryTest, OrphanedConnectionRemoveIsNoop) {
    // remove_subscriber on the orphaned connection must not crash or corrupt state.
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    registry_.remove_subscriber(ConnectionID{1});
    EXPECT_EQ(registry_.subscriber_count(), 1u);
    EXPECT_EQ(registry_.min_cursor(), 60);
}

TEST_F(ExternalWalSubscriberRegistryTest, OrphanReplacedByFreshSubscriberForSameId) {
    // A third reconnect with the same subscriber_id displaces the second.
    registry_.register_subscriber(ConnectionID{1}, "mep_primary", 50);
    registry_.register_subscriber(ConnectionID{2}, "mep_primary", 60);
    const ConnectionID orphan = registry_.register_subscriber(ConnectionID{3}, "mep_primary", 70);
    EXPECT_TRUE(orphan.is_valid());
    EXPECT_EQ(orphan, ConnectionID{2});
    EXPECT_EQ(registry_.subscriber_count(), 1u);
    EXPECT_EQ(registry_.min_cursor(), 70);
}

} // namespaces
