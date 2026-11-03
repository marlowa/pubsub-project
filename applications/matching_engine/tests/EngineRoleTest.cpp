// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "EngineRole.hpp"

namespace matching_engine::tests {

using Situation = EngineRole::Situation;
using Completed = EngineRole::Completed;

TEST(EngineRoleTest, AnInstanceStartsNeitherLeadingNorFollowing) {
    const EngineRole role(true);
    EXPECT_EQ(role.situation(), Situation::starting);
    EXPECT_EQ(role.role(), MeRole::Unknown);
    EXPECT_FALSE(role.catching_up());
    EXPECT_FALSE(role.following_the_wal());
    EXPECT_FALSE(role.keeps_its_reports());
    EXPECT_FALSE(role.catch_up_completed());
}

TEST(EngineRoleTest, CatchingUpToBecomeCurrentKeepsItsReportsAndEndsFollowingTheWal) {
    EngineRole role(true);
    role.start_catching_up_to_become_current();
    role.request_sent(false);
    EXPECT_EQ(role.role(), MeRole::Reconciling);
    EXPECT_TRUE(role.keeps_its_reports()) << "the request asked to follow";
    EXPECT_FALSE(role.following_the_wal()) << "not while catching up";
    EXPECT_TRUE(role.holding_reports_until_entitled());
    EXPECT_EQ(role.catch_up_completed_now(), Completed::became_current);
    EXPECT_EQ(role.situation(), Situation::current);
    EXPECT_EQ(role.role(), MeRole::Unknown);
    EXPECT_TRUE(role.following_the_wal());
    EXPECT_TRUE(role.catch_up_completed());
}

TEST(EngineRoleTest, WithHighAvailabilityOffTheOneInstanceNeitherFollowsNorHolds) {
    EngineRole role(false);
    role.start_catching_up_to_become_current();
    role.request_sent(true);
    EXPECT_FALSE(role.holding_reports_until_entitled());
    EXPECT_FALSE(role.keeps_its_reports());
    EXPECT_EQ(role.catch_up_completed_now(), Completed::became_current);
    EXPECT_FALSE(role.following_the_wal());
}

TEST(EngineRoleTest, ACatchUpToLeadEndsCurrentUntilTheLeaderRoleIsAdopted) {
    EngineRole role(true);
    role.start_catching_up_to_lead();
    role.request_sent(true);
    EXPECT_TRUE(role.catching_up_to_lead());
    EXPECT_FALSE(role.holding_reports_until_entitled()) << "an instance told it leads sends its reports";
    EXPECT_FALSE(role.keeps_its_reports());
    EXPECT_EQ(role.catch_up_completed_now(), Completed::to_lead);
    EXPECT_EQ(role.role(), MeRole::Unknown);
    role.became_leader();
    EXPECT_EQ(role.role(), MeRole::Leader);
    EXPECT_FALSE(role.following_the_wal());
}

TEST(EngineRoleTest, ACatchUpToFollowAgainGoesBackToWhereItWas) {
    EngineRole current(true);
    current.start_catching_up_to_become_current();
    current.request_sent(false);
    static_cast<void>(current.catch_up_completed_now());
    current.start_catching_up_to_follow_again();
    EXPECT_EQ(current.role(), MeRole::Reconciling);
    EXPECT_EQ(current.catch_up_completed_now(), Completed::following_again);
    EXPECT_EQ(current.situation(), Situation::current);

    EngineRole following(true);
    following.start_catching_up_to_become_current();
    following.request_sent(false);
    static_cast<void>(following.catch_up_completed_now());
    following.became_follower();
    following.start_catching_up_to_follow_again();
    EXPECT_EQ(following.catch_up_completed_now(), Completed::following_again);
    EXPECT_EQ(following.situation(), Situation::following);
    EXPECT_TRUE(following.following_the_wal());
}

TEST(EngineRoleTest, ALeaderNeverKeepsItsReports) {
    // A leader's latest request always asked to lead, so nothing reaches this combination: the role checks for leading
    // as well, so that a leader's reports are sent whatever the latest request asked.
    EngineRole role(true);
    role.start_catching_up_to_lead();
    role.request_sent(false);
    static_cast<void>(role.catch_up_completed_now());
    role.became_leader();
    EXPECT_FALSE(role.keeps_its_reports());
    EXPECT_FALSE(role.following_the_wal());
}

TEST(EngineRoleTest, ALeaderThatStopsLeadingFollowsAgainAsAFollower) {
    EngineRole role(true);
    role.start_catching_up_to_lead();
    role.request_sent(true);
    static_cast<void>(role.catch_up_completed_now());
    role.became_leader();
    role.became_follower();
    role.start_catching_up_to_follow_again();
    role.request_sent(false);
    EXPECT_EQ(role.catch_up_completed_now(), Completed::following_again);
    EXPECT_EQ(role.situation(), Situation::following);
    EXPECT_TRUE(role.following_the_wal());
}

TEST(EngineRoleTest, ALeaseGrantedWhileCatchingUpToLeadReplacesAFollowAgain) {
    EngineRole role(true);
    role.start_catching_up_to_become_current();
    role.request_sent(false);
    static_cast<void>(role.catch_up_completed_now());
    role.start_catching_up_to_follow_again();
    role.start_catching_up_to_lead();
    role.request_sent(true);
    EXPECT_EQ(role.catch_up_completed_now(), Completed::to_lead) << "the answer waited for is the one to lead (BUG-0218)";
}

TEST(EngineRoleTest, APeerAgreedToLeadWhileCatchingUpIsActedOnOnce) {
    EngineRole role(true);
    EXPECT_FALSE(role.take_peer_leader_note().has_value());
    role.note_peer_leader(2, 41);
    const auto noted = role.take_peer_leader_note();
    ASSERT_TRUE(noted.has_value());
    EXPECT_EQ(noted->instance_id, 2);
    EXPECT_EQ(noted->epoch, 41);
    EXPECT_FALSE(role.take_peer_leader_note().has_value());
}

TEST(EngineRoleTest, AFollowerThatNeverAskedToFollowDoesNotFollowTheWal) {
    // The defect BUG-0225, kept as it is: an instance restarted beside a leading peer agrees that the peer leads before
    // any sequencer has connected, and becomes a follower without having asked to follow the WAL.
    EngineRole role(true);
    role.became_follower();
    EXPECT_EQ(role.role(), MeRole::Follower);
    EXPECT_TRUE(role.follows_without_following_the_wal());
    EXPECT_FALSE(role.following_the_wal());
    EXPECT_FALSE(role.keeps_its_reports());
}

TEST(EngineRoleTest, ACommandDiscardedWithoutALeaseMeansTheInstanceIsNotCurrent) {
    EngineRole role(true);
    role.start_catching_up_to_become_current();
    static_cast<void>(role.catch_up_completed_now());
    ASSERT_TRUE(role.catch_up_completed());
    role.command_discarded_without_a_lease();
    EXPECT_FALSE(role.catch_up_completed());
}

} // namespaces
