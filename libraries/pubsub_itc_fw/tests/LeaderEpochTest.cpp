// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include <LeaderEpoch.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

using fix_common::LeaderEpoch;

namespace {

constexpr int64_t primary = 1;
constexpr int64_t secondary = 2;

} // un-named namespace

TEST(LeaderEpochTest, TheNextEpochIsAlwaysAboveTheOneKnown) {
    for (int32_t above = 0; above < 64; ++above) {
        EXPECT_GT(LeaderEpoch::next_for(above, primary), above);
        EXPECT_GT(LeaderEpoch::next_for(above, secondary), above);
    }
}

TEST(LeaderEpochTest, TheNextEpochRecordsItsLeader) {
    for (int32_t above = 0; above < 64; ++above) {
        EXPECT_EQ(LeaderEpoch::next_for(above, primary) % LeaderEpoch::epoch_stride, primary);
        EXPECT_EQ(LeaderEpoch::next_for(above, secondary) % LeaderEpoch::epoch_stride, secondary);
    }
}

// The property the whole scheme exists for: two issuers starting from the same known epoch, one
// making the primary leader and one the secondary, never produce the same epoch.
TEST(LeaderEpochTest, DifferentLeadersNeverShareAnEpoch) {
    for (int32_t above = 0; above < 64; ++above) {
        EXPECT_NE(LeaderEpoch::next_for(above, primary), LeaderEpoch::next_for(above, secondary));
    }
}

// The next epoch is the smallest that qualifies, so generations are not skipped needlessly.
TEST(LeaderEpochTest, TheNextEpochIsTheSmallestThatQualifies) {
    EXPECT_EQ(LeaderEpoch::next_for(0, primary), 1);
    EXPECT_EQ(LeaderEpoch::next_for(0, secondary), 2);
    EXPECT_EQ(LeaderEpoch::next_for(1, primary), 5);
    EXPECT_EQ(LeaderEpoch::next_for(1, secondary), 2);
    EXPECT_EQ(LeaderEpoch::next_for(2, primary), 5);
    EXPECT_EQ(LeaderEpoch::next_for(5, secondary), 6);
    EXPECT_EQ(LeaderEpoch::next_for(6, secondary), 10);
}

// An epoch stored before generations recorded their leader carries on: the next one is simply
// above it.
TEST(LeaderEpochTest, AnEpochFromBeforeTheSchemeIsCarriedForward) {
    EXPECT_EQ(LeaderEpoch::next_for(224, primary), 225);
    EXPECT_EQ(LeaderEpoch::next_for(224, secondary), 226);
    EXPECT_EQ(LeaderEpoch::next_for(223, primary), 225);
}

TEST(LeaderEpochTest, AnInstanceIdOutsideTheSchemeIsRefused) {
    EXPECT_THROW(EXPECT_GT(LeaderEpoch::next_for(0, 0), 0), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(EXPECT_GT(LeaderEpoch::next_for(0, LeaderEpoch::epoch_stride), 0), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LeaderEpochTest, ANegativeEpochIsRefused) {
    EXPECT_THROW(EXPECT_GT(LeaderEpoch::next_for(-1, primary), 0), pubsub_itc_fw::PreconditionAssertion);
}
