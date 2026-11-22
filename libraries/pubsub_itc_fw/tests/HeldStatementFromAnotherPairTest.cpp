// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <pubsub_itc_fw/HeldStatementFromAnotherPair.hpp>

namespace {

using pubsub_itc_fw::HeldStatementFromAnotherPair;
using Statement = HeldStatementFromAnotherPair::Statement;

constexpr bool may_not_lead = false;
constexpr bool may_lead = true;

TEST(HeldStatementFromAnotherPairTest, AStatementThatAnInstanceMayNotLeadBarsIt) {
    HeldStatementFromAnotherPair statements;
    EXPECT_EQ(statements.barred_instance(), 0);
    EXPECT_TRUE(statements.offer(Statement{5, 1, 2, may_not_lead}));
    EXPECT_EQ(statements.barred_instance(), 2);
    EXPECT_FALSE(statements.offer(Statement{5, 2, 2, may_not_lead})) << "the same instance stays barred";
    EXPECT_TRUE(statements.offer(Statement{5, 3, 2, may_lead}));
    EXPECT_EQ(statements.barred_instance(), 0);
}

TEST(HeldStatementFromAnotherPairTest, ALateCopyOfAnOlderStatementDoesNotUndoANewerOne) {
    HeldStatementFromAnotherPair statements;
    // Kept, though no instance was barred before or after, so the barred instance did not change.
    EXPECT_FALSE(statements.offer(Statement{5, 2, 2, may_lead}));
    ASSERT_TRUE(statements.held().has_value());
    EXPECT_EQ(statements.held()->number, 2);
    EXPECT_FALSE(statements.offer(Statement{5, 1, 2, may_not_lead}));
    EXPECT_EQ(statements.barred_instance(), 0);
    EXPECT_FALSE(statements.offer(Statement{5, 2, 2, may_not_lead})) << "the same number again is not newer";
    EXPECT_EQ(statements.barred_instance(), 0);
}

TEST(HeldStatementFromAnotherPairTest, AStatementFromAnEarlierLeadershipIsRefused) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.offer(Statement{9, 1, 2, may_not_lead}));
    // A maker that led at epoch 5 no longer leads: whatever number it gives, it cannot override the leader at epoch 9.
    EXPECT_FALSE(statements.offer(Statement{5, 100, 2, may_lead}));
    EXPECT_EQ(statements.barred_instance(), 2);
    statements.maker_epoch_known(13);
    EXPECT_FALSE(statements.offer(Statement{9, 2, 1, may_not_lead})) << "nor can the one at 9 once a later leadership is known";
}

TEST(HeldStatementFromAnotherPairTest, ALaterLeadershipOfTheMakerDropsTheStatement) {
    HeldStatementFromAnotherPair statements;
    EXPECT_TRUE(statements.offer(Statement{5, 1, 2, may_not_lead}));
    EXPECT_FALSE(statements.maker_epoch_known(5)) << "the epoch the statement was made at changes nothing";
    EXPECT_TRUE(statements.maker_epoch_known(9));
    EXPECT_EQ(statements.barred_instance(), 0);
    EXPECT_FALSE(statements.held().has_value());
    // The new leadership starts with no statement standing, and may make its own.
    EXPECT_TRUE(statements.offer(Statement{9, 1, 1, may_not_lead}));
    EXPECT_EQ(statements.barred_instance(), 1);
}

} // namespaces
