// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include "LeadershipDecision.hpp"

using arbiter::LeadershipDecision;

namespace {

// The venue's shape: the primary is instance 1 and the secondary instance 2, permanently.
// Which of them leads is a separate question and is what this class answers.
constexpr int64_t primary = 1;
constexpr int64_t secondary = 2;

// A pair that has never been arbitrated, asked by one of them.
LeadershipDecision::Inputs cold_start(int64_t asked_by, int64_t peer) {
    LeadershipDecision::Inputs inputs;
    inputs.self_instance_id = asked_by;
    inputs.peer_instance_id = peer;
    inputs.peer_connected = true;
    inputs.has_incumbent = false;
    inputs.reported_epoch = 0;
    return inputs;
}

// A pair with a leader already on record.
LeadershipDecision::Inputs with_incumbent(int64_t asked_by, int64_t peer, int64_t incumbent, bool incumbent_connected, int32_t incumbent_epoch) {
    LeadershipDecision::Inputs inputs;
    inputs.self_instance_id = asked_by;
    inputs.peer_instance_id = peer;
    inputs.peer_connected = incumbent_connected && incumbent == peer;
    inputs.has_incumbent = true;
    inputs.incumbent_instance_id = incumbent;
    inputs.incumbent_connected = incumbent_connected;
    inputs.incumbent_epoch = incumbent_epoch;
    inputs.reported_epoch = incumbent_epoch;
    return inputs;
}

} // un-named namespace

// -- Cold start: the lowest id preference, which is what it is actually for -----

TEST(LeadershipDecisionTest, ColdStartPrefersTheLowerInstanceId) {
    const LeadershipDecision decision = LeadershipDecision::decide(cold_start(secondary, primary));
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_EQ(decision.follower_instance_id, secondary);
    EXPECT_TRUE(decision.leadership_changed);
}

// The answer must not depend on which of the two happened to ask, since at startup they come
// up in either order and either may get there first.
TEST(LeadershipDecisionTest, ColdStartGivesTheSameAnswerWhicheverInstanceAsks) {
    const LeadershipDecision asked_by_primary = LeadershipDecision::decide(cold_start(primary, secondary));
    const LeadershipDecision asked_by_secondary = LeadershipDecision::decide(cold_start(secondary, primary));
    EXPECT_EQ(asked_by_primary.leader_instance_id, asked_by_secondary.leader_instance_id);
    EXPECT_EQ(asked_by_primary.follower_instance_id, asked_by_secondary.follower_instance_id);
}

// With nobody else there, the instance asking is the only candidate. This is the secondary
// starting alone, and it is also the primary restarting to find the secondary's machine gone.
TEST(LeadershipDecisionTest, TheOnlyConnectedInstanceLeadsWhateverItsId) {
    LeadershipDecision::Inputs inputs = cold_start(secondary, primary);
    inputs.peer_connected = false;
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    EXPECT_EQ(decision.leader_instance_id, secondary);
    EXPECT_EQ(decision.follower_instance_id, primary);
}

// -- Rejoin: the case the lowest-id preference used to get wrong --------------
//
// These are the tests that fail against the previous rule, where leadership was recomputed
// from instance ids every time and a restarted primary took it back from a working secondary.

TEST(LeadershipDecisionTest, ARestartedPrimaryDoesNotTakeLeadershipFromAConnectedSecondary) {
    // The secondary was promoted while the primary was down; the primary is now back and asks.
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, true, 4));
    EXPECT_EQ(decision.leader_instance_id, secondary) << "the primary took leadership back from a healthy secondary";
    EXPECT_EQ(decision.follower_instance_id, primary);
    EXPECT_FALSE(decision.leadership_changed);
}

// Confirming the incumbent must not move the epoch. Epochs are checked on every PDU, so
// superseding the leader's own epoch would have its traffic discarded while it is doing
// nothing wrong.
TEST(LeadershipDecisionTest, ConfirmingTheIncumbentLeavesTheEpochAlone) {
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, true, 4));
    EXPECT_EQ(decision.epoch, 4);
}

// The secondary asking about itself gets the same answer as the primary asking about it.
TEST(LeadershipDecisionTest, TheIncumbentIsConfirmedWhicheverInstanceAsks) {
    const LeadershipDecision asked_by_leader = LeadershipDecision::decide(with_incumbent(secondary, primary, secondary, true, 4));
    EXPECT_EQ(asked_by_leader.leader_instance_id, secondary);
    EXPECT_EQ(asked_by_leader.follower_instance_id, primary);
    EXPECT_FALSE(asked_by_leader.leadership_changed);
}

// An incumbent that is no longer connected has no claim: its machine has gone and the survivor
// must take over. This is the second failure in a row, which the pair has to survive.
TEST(LeadershipDecisionTest, ADisconnectedIncumbentLosesLeadershipToTheSurvivor) {
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, false, 4));
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_EQ(decision.follower_instance_id, secondary);
    EXPECT_TRUE(decision.leadership_changed);
}

TEST(LeadershipDecisionTest, ChangingLeadershipAdvancesTheEpoch) {
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, false, 4));
    EXPECT_EQ(decision.epoch, 5);
}

// A restart inside the follower's grace period: the secondary never promoted, so the primary is
// still the recorded leader and simply resumes. With a supervisor this is the common case.
TEST(LeadershipDecisionTest, APrimaryThatWasNeverReplacedResumesLeadership) {
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, primary, true, 2));
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_EQ(decision.follower_instance_id, secondary);
    EXPECT_FALSE(decision.leadership_changed);
    EXPECT_EQ(decision.epoch, 2);
}

// -- Epoch monotonicity --------------------------------------------------------
//
// An instance that has been away comes back believing an older epoch is in force. Its report
// must not be able to wind the sequence backwards, or a later decision could reuse an epoch
// number that has already been superseded.
TEST(LeadershipDecisionTest, AStaleReportedEpochCannotWindTheSequenceBack) {
    LeadershipDecision::Inputs inputs = with_incumbent(primary, secondary, secondary, false, 9);
    inputs.reported_epoch = 2; // the restarted instance is behind
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    EXPECT_EQ(decision.epoch, 10) << "the arbiter's own record should have won";
}

TEST(LeadershipDecisionTest, AnAheadReportedEpochIsRespected) {
    LeadershipDecision::Inputs inputs = with_incumbent(primary, secondary, secondary, false, 3);
    inputs.reported_epoch = 7;
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    EXPECT_EQ(decision.epoch, 8);
}

// -- The property that matters most -------------------------------------------
//
// Whatever the inputs, the pair must never be told the same instance is both leader and
// follower, and the follower must always be the other one of the two.
TEST(LeadershipDecisionTest, LeaderAndFollowerAreAlwaysTheTwoDistinctInstances) {
    for (int64_t asked_by : {primary, secondary}) {
        for (bool has_incumbent : {false, true}) {
            for (int64_t incumbent : {primary, secondary}) {
                for (bool incumbent_connected : {false, true}) {
                    for (bool peer_connected : {false, true}) {
                        LeadershipDecision::Inputs inputs;
                        inputs.self_instance_id = asked_by;
                        inputs.peer_instance_id = (asked_by == primary) ? secondary : primary;
                        inputs.peer_connected = peer_connected;
                        inputs.has_incumbent = has_incumbent;
                        inputs.incumbent_instance_id = incumbent;
                        inputs.incumbent_connected = incumbent_connected;
                        const LeadershipDecision decision = LeadershipDecision::decide(inputs);
                        EXPECT_NE(decision.leader_instance_id, decision.follower_instance_id);
                        EXPECT_TRUE(decision.leader_instance_id == primary || decision.leader_instance_id == secondary);
                        EXPECT_TRUE(decision.follower_instance_id == primary || decision.follower_instance_id == secondary);
                    }
                }
            }
        }
    }
}
