// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <pubsub_itc_fw/HeldStatementFromAnotherPair.hpp>
#include <pubsub_itc_fw/InstancesBarredFromLeading.hpp>

namespace {

using pubsub_itc_fw::HeldStatementFromAnotherPair;
using pubsub_itc_fw::InstancesBarredFromLeading;
using Statement = HeldStatementFromAnotherPair::Statement;

const InstancesBarredFromLeading none_barred{};
const InstancesBarredFromLeading first_barred{true, false};
const InstancesBarredFromLeading second_barred{false, true};
const InstancesBarredFromLeading both_barred{true, true};

TEST(HeldStatementFromAnotherPairTest, AStatementThatAnInstanceMayNotLeadBarsIt) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.barred_instances().empty());
    EXPECT_TRUE(statements.offer(Statement{5, 1, second_barred}));
    EXPECT_TRUE(statements.barred_instances().contains(2));
    EXPECT_FALSE(statements.barred_instances().contains(1));
    EXPECT_FALSE(statements.offer(Statement{5, 2, second_barred})) << "the same instance stays barred";
    EXPECT_TRUE(statements.offer(Statement{5, 3, none_barred}));
    EXPECT_TRUE(statements.barred_instances().empty());
}

TEST(HeldStatementFromAnotherPairTest, BothInstancesCanBeBarredAtOnce) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.offer(Statement{5, 1, first_barred}));
    // The second engine took over, and was judged stopped in turn: the first must stay barred.
    EXPECT_TRUE(statements.offer(Statement{5, 2, both_barred}));
    EXPECT_TRUE(statements.barred_instances().contains(1));
    EXPECT_TRUE(statements.barred_instances().contains(2));
}

TEST(HeldStatementFromAnotherPairTest, ALateCopyOfAnOlderStatementDoesNotUndoANewerOne) {
    HeldStatementFromAnotherPair statements;
    // Kept, though no instance was barred before or after, so the barred instances did not change.
    EXPECT_FALSE(statements.offer(Statement{5, 2, none_barred}));
    ASSERT_TRUE(statements.held().has_value());
    EXPECT_EQ(statements.held()->number, 2);
    EXPECT_FALSE(statements.offer(Statement{5, 1, second_barred}));
    EXPECT_TRUE(statements.barred_instances().empty());
    EXPECT_FALSE(statements.offer(Statement{5, 2, second_barred})) << "the same number again is not newer";
    EXPECT_TRUE(statements.barred_instances().empty());
}

TEST(HeldStatementFromAnotherPairTest, AStatementFromAnEarlierLeadershipIsRefused) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.offer(Statement{9, 1, second_barred}));
    // A maker that led at epoch 5 no longer leads: whatever number it gives, it cannot override the leader at epoch 9.
    EXPECT_FALSE(statements.offer(Statement{5, 100, none_barred}));
    EXPECT_TRUE(statements.barred_instances().contains(2));
    statements.maker_epoch_known(13);
    EXPECT_FALSE(statements.offer(Statement{9, 2, first_barred})) << "nor can the one at 9 once a later leadership is known";
}

TEST(HeldStatementFromAnotherPairTest, ALaterLeadershipOfTheMakerDropsTheStatement) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.offer(Statement{5, 1, second_barred}));
    EXPECT_FALSE(statements.maker_epoch_known(5)) << "the epoch the statement was made at changes nothing";
    EXPECT_TRUE(statements.maker_epoch_known(9));
    EXPECT_TRUE(statements.barred_instances().empty());
    EXPECT_FALSE(statements.held().has_value());
    // The new leadership starts with no statement standing, and may make its own.
    EXPECT_TRUE(statements.offer(Statement{9, 1, first_barred}));
    EXPECT_TRUE(statements.barred_instances().contains(1));
}

} // namespaces
