// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include <LeaderStatement.hpp>
#include <LeaseHolder.hpp>
#include <LeaseParticipant.hpp>
#include <LeaseVoter.hpp>
#include <PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds lease_period{3000};
constexpr milliseconds drift_allowance{250};
const Clock::time_point start{};
const fix_common::PeerStatementsFlag no_statements{fix_common::PeerStatementsFlag::PeerAlwaysMayLead};
const fix_common::PeerStatementsFlag statements{fix_common::PeerStatementsFlag::SayWhetherPeerMayLead};

// Makes instance 1 the leader at epoch 1 with the arbiter's grant, at start + lease_period.
void make_first_instance_lead(fix_common::LeaseParticipant& participant) {
    const Clock::time_point now = start + lease_period;
    ASSERT_EQ(participant.begin_asking_to_lead(), 1);
    const int64_t request = participant.record_request(3, now);
    ASSERT_EQ(participant.on_grant(3, request, 1, now), fix_common::LeaseHolder::Event::BecameLeader);
}

} // un-named namespace

TEST(LeaseRulesTest, VoterGrantsNothingForOneLeasePeriodAfterStarting) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    EXPECT_EQ(voter.consider(1, 1, start + milliseconds{2999}).verdict, fix_common::LeaseVoter::Verdict::RefusedWhileRestarting);
    EXPECT_EQ(voter.consider(1, 1, start + lease_period).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, VoterGrantsToOneInstanceUntilThePromiseRunsOut) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    const Clock::time_point granted_at = start + lease_period;
    EXPECT_EQ(voter.consider(1, 1, granted_at).verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voter.consider(2, 2, granted_at + milliseconds{2999}).verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
    EXPECT_EQ(voter.consider(2, 2, granted_at + lease_period).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, VoterRenewsTheInstanceItHasPromisedTo) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    const Clock::time_point granted_at = start + lease_period;
    EXPECT_EQ(voter.consider(1, 1, granted_at).verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voter.consider(1, 1, granted_at + milliseconds{1000}).verdict, fix_common::LeaseVoter::Verdict::Granted);
    // The renewal moved the promise on, so the other instance is refused past the first expiry.
    EXPECT_EQ(voter.consider(2, 2, granted_at + milliseconds{3500}).verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
}

TEST(LeaseRulesTest, VoterNeverGrantsAnEpochBelowOneItHasGranted) {
    fix_common::LeaseVoter voter(lease_period, start, 6);
    const fix_common::LeaseVoter::Answer answer = voter.consider(1, 5, start + lease_period);
    EXPECT_EQ(answer.verdict, fix_common::LeaseVoter::Verdict::RefusedEpochBehind);
    EXPECT_EQ(answer.highest_epoch, 6);
}

TEST(LeaseRulesTest, VoterHeldForItsOwnInstanceGrantsNothingElse) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    voter.hold_for_self();
    EXPECT_EQ(voter.consider(2, 2, start + milliseconds{60000}).verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
    voter.release_self();
    EXPECT_EQ(voter.consider(2, 2, start + milliseconds{60000}).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, VoterRejectsANonPositiveLeasePeriod) {
    EXPECT_THROW(fix_common::LeaseVoter(milliseconds{0}, start, 0), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LeaseRulesTest, HolderCountsItsLeaseFromWhenTheRequestWasSent) {
    fix_common::LeaseHolder holder(lease_period, drift_allowance);
    holder.begin_candidacy(1);
    const int64_t request = holder.record_request(3, start);
    // The grant takes two seconds to arrive. The lease still runs from the send, less the drift allowance.
    EXPECT_EQ(holder.on_grant(3, request, 1, start + milliseconds{2000}), fix_common::LeaseHolder::Event::BecameLeader);
    EXPECT_EQ(holder.lease_expires_at(), start + lease_period - drift_allowance);
    EXPECT_TRUE(holder.acting(start + milliseconds{2749}));
    EXPECT_FALSE(holder.acting(start + milliseconds{2750}));
}

TEST(LeaseRulesTest, HolderIgnoresAGrantThatArrivesAfterItsLeaseWouldHaveEnded) {
    fix_common::LeaseHolder holder(lease_period, drift_allowance);
    holder.begin_candidacy(1);
    const int64_t request = holder.record_request(3, start);
    EXPECT_EQ(holder.on_grant(3, request, 1, start + milliseconds{2900}), fix_common::LeaseHolder::Event::Nothing);
    EXPECT_EQ(holder.state(), fix_common::LeaseHolder::State::Candidate);
}

TEST(LeaseRulesTest, HolderIgnoresAGrantForAnotherEpochOrFromTheWrongVoter) {
    fix_common::LeaseHolder holder(lease_period, drift_allowance);
    holder.begin_candidacy(5);
    const int64_t request = holder.record_request(3, start);
    EXPECT_EQ(holder.on_grant(2, request, 5, start), fix_common::LeaseHolder::Event::Nothing);
    const int64_t second = holder.record_request(3, start);
    EXPECT_EQ(holder.on_grant(3, second, 1, start), fix_common::LeaseHolder::Event::Nothing);
    EXPECT_EQ(holder.state(), fix_common::LeaseHolder::State::Candidate);
}

TEST(LeaseRulesTest, HolderLearnsOfANewerEpochFromARefusal) {
    fix_common::LeaseHolder holder(lease_period, drift_allowance);
    holder.begin_candidacy(5);
    const int64_t request = holder.record_request(3, start);
    EXPECT_EQ(holder.on_refusal(request, 6), fix_common::LeaseHolder::Event::NewerEpochKnown);
    const int64_t second = holder.record_request(3, start);
    EXPECT_EQ(holder.on_refusal(second, 5), fix_common::LeaseHolder::Event::Nothing);
}

TEST(LeaseRulesTest, HolderRejectsADriftAllowanceAsLongAsTheLease) {
    EXPECT_THROW(fix_common::LeaseHolder(lease_period, lease_period), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LeaseRulesTest, ParticipantAsksInTheNextEpochInWhichItLeads) {
    fix_common::LeaseParticipant participant(2, lease_period, drift_allowance, start, 5, no_statements);
    ASSERT_TRUE(participant.may_ask_to_lead(start + lease_period));
    EXPECT_EQ(participant.begin_asking_to_lead(), 6);
}

TEST(LeaseRulesTest, ParticipantDoesNotAskToLeadWhileItsGrantToThePeerIsLive) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point granted_at = start + lease_period;
    EXPECT_EQ(participant.on_peer_request(2, 2, granted_at).answer.verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_FALSE(participant.may_ask_to_lead(granted_at + milliseconds{2999}));
    EXPECT_TRUE(participant.may_ask_to_lead(granted_at + lease_period));
}

TEST(LeaseRulesTest, ParticipantAskingToLeadRefusesItsPeer) {
    fix_common::LeaseParticipant participant(2, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(participant.begin_asking_to_lead(), 2);
    const auto outcome = participant.on_peer_request(1, 1, now);
    EXPECT_EQ(outcome.answer.verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
    EXPECT_FALSE(outcome.gave_up_asking_to_lead);
}

TEST(LeaseRulesTest, ParticipantAskingToLeadGivesWayToAHigherEpoch) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(participant.begin_asking_to_lead(), 1);
    const auto outcome = participant.on_peer_request(2, 2, now);
    EXPECT_TRUE(outcome.gave_up_asking_to_lead);
    EXPECT_EQ(outcome.answer.verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(participant.state(), fix_common::LeaseHolder::State::Idle);
}

TEST(LeaseRulesTest, LeaderDoesNotGiveWayToItsPeer) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(participant.begin_asking_to_lead(), 1);
    const int64_t request = participant.record_request(3, now);
    EXPECT_EQ(participant.on_grant(3, request, 1, now), fix_common::LeaseHolder::Event::BecameLeader);
    const auto outcome = participant.on_peer_request(2, 6, now);
    EXPECT_FALSE(outcome.gave_up_asking_to_lead);
    EXPECT_EQ(outcome.answer.verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
    EXPECT_TRUE(participant.acting(now));
}

TEST(LeaseRulesTest, ParticipantStopsWhenItsLeaseRunsOut) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(participant.begin_asking_to_lead(), 1);
    const int64_t request = participant.record_request(3, now);
    EXPECT_EQ(participant.on_grant(3, request, 1, now), fix_common::LeaseHolder::Event::BecameLeader);
    EXPECT_FALSE(participant.stop_if_lease_ran_out(now + milliseconds{2749}));
    EXPECT_TRUE(participant.stop_if_lease_ran_out(now + milliseconds{2750}));
    EXPECT_EQ(participant.state(), fix_common::LeaseHolder::State::Idle);
    // Having stopped, it no longer votes for itself, so its peer can now be granted.
    EXPECT_EQ(participant.on_peer_request(2, 2, now + milliseconds{2750}).answer.verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, ParticipantStopsAndWaitsOnLearningOfANewerEpoch) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(participant.begin_asking_to_lead(), 1);
    const int64_t request = participant.record_request(3, now);
    EXPECT_EQ(participant.on_refusal(request, 6, now, milliseconds{500}), fix_common::LeaseHolder::Event::NewerEpochKnown);
    EXPECT_EQ(participant.state(), fix_common::LeaseHolder::State::Idle);
    EXPECT_EQ(participant.highest_epoch(), 6);
    EXPECT_FALSE(participant.may_ask_to_lead(now + milliseconds{499}));
    ASSERT_TRUE(participant.may_ask_to_lead(now + milliseconds{500}));
    EXPECT_EQ(participant.begin_asking_to_lead(), 9);
}

TEST(LeaseRulesTest, TwoInstancesAskingAtOnceWithNoArbiterSettleOnOneLeader) {
    // Both ask at the same moment and the arbiter is down, so each has only the other's vote to win.
    // Without the rule that a candidate gives way to a higher epoch, each refuses the other and the
    // same thing can happen on every attempt (docs/availability/tla/traces/lease-6-no-yield-livelock.txt).
    const Clock::time_point now = start + lease_period;
    fix_common::LeaseParticipant first(1, lease_period, drift_allowance, start, 0, no_statements);
    fix_common::LeaseParticipant second(2, lease_period, drift_allowance, start, 0, no_statements);
    const int32_t first_epoch = first.begin_asking_to_lead();
    const int32_t second_epoch = second.begin_asking_to_lead();
    const int64_t first_request = first.record_request(2, now);
    const int64_t second_request = second.record_request(1, now);

    const auto first_answer = first.on_peer_request(2, second_epoch, now);
    const auto second_answer = second.on_peer_request(1, first_epoch, now);

    for (const auto& reply : {std::make_pair(&first, &second_answer), std::make_pair(&second, &first_answer)}) {
        fix_common::LeaseParticipant& asker = *reply.first;
        const int64_t request = &asker == &first ? first_request : second_request;
        const int64_t voter = &asker == &first ? 2 : 1;
        if (reply.second->answer.verdict == fix_common::LeaseVoter::Verdict::Granted) {
            static_cast<void>(asker.on_grant(voter, request, asker.epoch(), now));
        } else {
            static_cast<void>(asker.on_refusal(request, reply.second->answer.highest_epoch, now, milliseconds{500}));
        }
    }
    EXPECT_NE(first.acting(now), second.acting(now));
    EXPECT_TRUE(second.acting(now));
}

TEST(LeaseRulesTest, VoterRefusesAnInstanceAStatementSaysMayNotLead) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    voter.record_statement(fix_common::LeaderStatement{1, 5, 1, false});
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(voter.consider(2, 6, now).verdict, fix_common::LeaseVoter::Verdict::RefusedMayNotLead);
    EXPECT_EQ(voter.consider(1, 9, now).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, VoterKeepsOnlyTheNewestStatement) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    const Clock::time_point now = start + lease_period;
    voter.record_statement(fix_common::LeaderStatement{1, 5, 2, false});
    // An older statement from the same leadership arrives late, and must not let instance 2 lead.
    voter.record_statement(fix_common::LeaderStatement{1, 5, 1, true});
    EXPECT_FALSE(voter.holds_statement(fix_common::LeaderStatement{1, 5, 1, true}));
    EXPECT_EQ(voter.consider(2, 6, now).verdict, fix_common::LeaseVoter::Verdict::RefusedMayNotLead);
    voter.record_statement(fix_common::LeaderStatement{1, 5, 3, true});
    EXPECT_TRUE(voter.holds_statement(fix_common::LeaderStatement{1, 5, 3, true}));
    EXPECT_EQ(voter.consider(2, 6, now).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(LeaseRulesTest, AStatementFromALaterEpochReplacesOneWithAHigherNumber) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    voter.record_statement(fix_common::LeaderStatement{1, 5, 7, false});
    voter.record_statement(fix_common::LeaderStatement{2, 6, 1, false});
    EXPECT_TRUE(voter.may_lead(2));
    EXPECT_FALSE(voter.may_lead(1));
}

TEST(LeaseRulesTest, VoterRejectsAStatementThatNamesNoLeader) {
    fix_common::LeaseVoter voter(lease_period, start, 0);
    EXPECT_THROW(voter.record_statement(fix_common::LeaderStatement{0, 5, 1, false}), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(voter.record_statement(fix_common::LeaderStatement{1, 5, 0, false}), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LeaseRulesTest, LeaderStartsEachLeadershipSayingItsPeerMayNotLead) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, statements);
    make_first_instance_lead(participant);
    EXPECT_FALSE(participant.statement().peer_may_lead);
    EXPECT_EQ(participant.statement().number, 1);
    EXPECT_EQ(participant.recorded_statement().instance_that_may_not_lead(), 2);
}

TEST(LeaseRulesTest, AnOldLeaderWhoseLeaseRanOutRefusesAPeerItsRecordSaysMayNotLead) {
    // An old leader that has not crashed, and whose lease has merely run out, would otherwise vote for a
    // peer lacking commands the engine acted on (docs/availability/tla/traces/behind-2-leader-keeps-no-record.txt).
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, statements);
    make_first_instance_lead(participant);
    const Clock::time_point later = start + lease_period + milliseconds{2750};
    ASSERT_TRUE(participant.stop_if_lease_ran_out(later));
    EXPECT_EQ(participant.on_peer_request(2, 2, later).answer.verdict, fix_common::LeaseVoter::Verdict::RefusedMayNotLead);
}

TEST(LeaseRulesTest, LeaderMayActWithoutItsPeerOnlyOnceAnotherVoterEchoesTheStatement) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, statements);
    make_first_instance_lead(participant);
    EXPECT_FALSE(participant.may_act_without_peer());
    participant.note_echo(5, 1);
    EXPECT_FALSE(participant.may_act_without_peer()) << "an echo for another epoch counts for nothing";
    participant.note_echo(1, 1);
    EXPECT_TRUE(participant.may_act_without_peer());

    participant.peer_holds_everything();
    EXPECT_TRUE(participant.statement().peer_may_lead);
    EXPECT_EQ(participant.statement().number, 2);
    EXPECT_FALSE(participant.may_act_without_peer());
    EXPECT_EQ(participant.recorded_statement().instance_that_may_not_lead(), 0);

    participant.begin_running_without_peer();
    EXPECT_EQ(participant.statement().number, 3);
    participant.note_echo(1, 2);
    EXPECT_FALSE(participant.may_act_without_peer()) << "an echo of an earlier statement must not count";
    participant.note_echo(1, 3);
    EXPECT_TRUE(participant.may_act_without_peer());
}

TEST(LeaseRulesTest, SayingTheSameThingAgainDoesNotChangeTheStatement) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, statements);
    make_first_instance_lead(participant);
    participant.begin_running_without_peer();
    EXPECT_EQ(participant.statement().number, 1);
    participant.peer_holds_everything();
    participant.peer_holds_everything();
    EXPECT_EQ(participant.statement().number, 2);
}

TEST(LeaseRulesTest, APairThatMakesNoStatementsNeverStopsItsPeerLeading) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, no_statements);
    make_first_instance_lead(participant);
    EXPECT_EQ(participant.statement().leader_id, 0);
    EXPECT_EQ(participant.recorded_statement().instance_that_may_not_lead(), 0);
    EXPECT_THROW(participant.begin_running_without_peer(), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_FALSE(participant.may_act_without_peer());
}

TEST(LeaseRulesTest, OnlyALeaderMakesStatements) {
    fix_common::LeaseParticipant participant(1, lease_period, drift_allowance, start, 0, statements);
    EXPECT_THROW(participant.begin_running_without_peer(), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(participant.peer_holds_everything(), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LeaseRulesTest, AnInstanceThatHoldsAStatementThatItMayNotLeadDoesNotAsk) {
    fix_common::LeaseParticipant participant(2, lease_period, drift_allowance, start, 0, statements);
    const Clock::time_point later = start + milliseconds{60000};
    EXPECT_EQ(participant.record_peer_statement(fix_common::LeaderStatement{1, 1, 2, false}), 2);
    EXPECT_FALSE(participant.may_ask_to_lead(later));
    EXPECT_EQ(participant.record_peer_statement(fix_common::LeaderStatement{1, 1, 1, true}), 0) << "a late statement is not echoed";
    EXPECT_FALSE(participant.may_ask_to_lead(later));
    EXPECT_EQ(participant.record_peer_statement(fix_common::LeaderStatement{1, 1, 3, true}), 3);
    EXPECT_TRUE(participant.may_ask_to_lead(later));
}

TEST(LeaseRulesTest, ARestoredStatementIsHonouredAfterARestart) {
    fix_common::LeaseParticipant participant(2, lease_period, drift_allowance, start, 1, statements);
    participant.restore_recorded_statement(fix_common::LeaderStatement{1, 1, 1, false});
    EXPECT_FALSE(participant.may_ask_to_lead(start + milliseconds{60000}));
}
