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

// The new epoch is the next one above the record's in which the new leader, the primary, leads:
// its remainder on division by 4 is 1.
TEST(LeadershipDecisionTest, ChangingLeadershipAdvancesTheEpoch) {
    const LeadershipDecision decision = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, false, 4));
    EXPECT_EQ(decision.epoch, 5);
}

TEST(LeadershipDecisionTest, TheNewEpochRecordsWhichInstanceLeads) {
    const LeadershipDecision to_primary = LeadershipDecision::decide(with_incumbent(primary, secondary, secondary, false, 4));
    const LeadershipDecision to_secondary = LeadershipDecision::decide(with_incumbent(secondary, primary, primary, false, 4));
    EXPECT_EQ(to_primary.epoch % fix_common::LeaderEpoch::epoch_stride, primary);
    EXPECT_EQ(to_secondary.epoch % fix_common::LeaderEpoch::epoch_stride, secondary);
    EXPECT_NE(to_primary.epoch, to_secondary.epoch) << "two different leaders must never be given the same epoch";
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
    // The next epoch above the record's 9 in which the primary leads.
    EXPECT_EQ(decision.epoch, 13) << "the arbiter's own record should have won";
}

TEST(LeadershipDecisionTest, AnAheadReportedEpochIsRespected) {
    LeadershipDecision::Inputs inputs = with_incumbent(primary, secondary, secondary, false, 3);
    inputs.reported_epoch = 7;
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    // The next epoch above the reported 7 in which the primary leads.
    EXPECT_EQ(decision.epoch, 9);
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

// -- The inputs the arbiter builds ---------------------------------------------
//
// These go through inputs_for, which is what ArbiterThread uses, so they test the inputs the
// arbiter really hands to decide() rather than inputs chosen by the test.

namespace {

LeadershipDecision::Inputs report_from(int64_t asked_by, bool peer_connected, int32_t reported_epoch) {
    LeadershipDecision::Inputs report;
    report.self_instance_id = asked_by;
    report.peer_instance_id = (asked_by == primary) ? secondary : primary;
    report.peer_connected = peer_connected;
    report.reported_epoch = reported_epoch;
    return report;
}

} // un-named namespace

// The case the arbiter once got wrong. The secondary led at 2 and has disconnected, so the record
// is no longer trusted to name the leader. The primary returns with the epoch it held before the
// secondary took over. The new generation must be above 2, the epoch the secondary led in, or two
// leaders share an epoch that receivers accept from both.
TEST(LeadershipDecisionTest, ARecordNoLongerTrustedStillBoundsTheNewEpoch) {
    LeadershipDecision::Record record;
    record.exists = true;
    record.trusted = false;
    record.leader_instance_id = secondary;
    record.leader_connected = false;
    record.epoch = 2;
    const LeadershipDecision decision = LeadershipDecision::decide(LeadershipDecision::inputs_for(report_from(primary, false, 1), record));
    EXPECT_EQ(decision.leader_instance_id, primary);
    // The next epoch above the record's 2 in which the primary leads.
    EXPECT_EQ(decision.epoch, 5) << "the epoch on record was issued, trusted or not, and the new one must be above it";
}

TEST(LeadershipDecisionTest, ARecordNoLongerTrustedIsNotTreatedAsAnIncumbent) {
    LeadershipDecision::Record record;
    record.exists = true;
    record.trusted = false;
    record.leader_instance_id = secondary;
    record.leader_connected = true;
    record.epoch = 2;
    const LeadershipDecision::Inputs inputs = LeadershipDecision::inputs_for(report_from(primary, true, 2), record);
    EXPECT_FALSE(inputs.has_incumbent);
    EXPECT_FALSE(inputs.incumbent_connected);
    EXPECT_EQ(inputs.incumbent_epoch, 2);
}

TEST(LeadershipDecisionTest, ATrustedConnectedRecordIsConfirmed) {
    LeadershipDecision::Record record;
    record.exists = true;
    record.trusted = true;
    record.leader_instance_id = secondary;
    record.leader_connected = true;
    record.epoch = 4;
    const LeadershipDecision decision = LeadershipDecision::decide(LeadershipDecision::inputs_for(report_from(primary, true, 4), record));
    EXPECT_EQ(decision.leader_instance_id, secondary);
    EXPECT_EQ(decision.epoch, 4);
    EXPECT_FALSE(decision.leadership_changed);
}

TEST(LeadershipDecisionTest, WithNoRecordTheReportedEpochIsTheBound) {
    const LeadershipDecision::Record no_record;
    const LeadershipDecision decision = LeadershipDecision::decide(LeadershipDecision::inputs_for(report_from(secondary, false, 5), no_record));
    EXPECT_EQ(decision.leader_instance_id, secondary);
    // The next epoch above the reported 5 in which the secondary leads.
    EXPECT_EQ(decision.epoch, 6);
}

// -- Lifting the incumbent -----------------------------------------------------
//
// A follower that promoted itself while cut off, then was told to follow again, holds an epoch
// above its leader's and discards the leader's heartbeats as stale. Its report must move the
// leader above it, keeping the same leader.
TEST(LeadershipDecisionTest, AReportAboveTheIncumbentLiftsItIntoANewerGeneration) {
    LeadershipDecision::Inputs inputs = with_incumbent(secondary, primary, primary, true, 5);
    inputs.reported_epoch = 6;
    inputs.lift_incumbent_above_report = true;
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_FALSE(decision.leadership_changed);
    EXPECT_EQ(decision.epoch, 9) << "the next epoch above the follower's 6 in which the primary leads";
}

TEST(LeadershipDecisionTest, WithoutLiftingAReportAboveTheIncumbentLeavesItsEpoch) {
    LeadershipDecision::Inputs inputs = with_incumbent(secondary, primary, primary, true, 5);
    inputs.reported_epoch = 6;
    const LeadershipDecision decision = LeadershipDecision::decide(inputs);
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_EQ(decision.epoch, 5);
}

TEST(LeadershipDecisionTest, AReportAtOrBelowTheIncumbentDoesNotLiftIt) {
    LeadershipDecision::Inputs inputs = with_incumbent(secondary, primary, primary, true, 5);
    inputs.reported_epoch = 5;
    inputs.lift_incumbent_above_report = true;
    EXPECT_EQ(LeadershipDecision::decide(inputs).epoch, 5);
}

TEST(LeadershipDecisionTest, InputsForCarriesTheLiftingChoiceThrough) {
    LeadershipDecision::Inputs report = report_from(secondary, true, 6);
    report.lift_incumbent_above_report = true;
    LeadershipDecision::Record record;
    record.exists = true;
    record.trusted = true;
    record.leader_instance_id = primary;
    record.leader_connected = true;
    record.epoch = 5;
    const LeadershipDecision decision = LeadershipDecision::decide(LeadershipDecision::inputs_for(report, record));
    EXPECT_EQ(decision.leader_instance_id, primary);
    EXPECT_EQ(decision.epoch, 9);
}
