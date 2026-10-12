// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ComponentLeaseVoters.hpp>

#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LeaseVoter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds lease_period{3000};
const Clock::time_point start{};
// Two pairs, by the numbers their instances put on their lease messages. Any positive numbers will do.
constexpr int32_t first_pair = 1;
constexpr int32_t second_pair = 2;

} // un-named namespace

TEST(ComponentLeaseVotersTest, APassiveArbiterMayNotAnswer) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    EXPECT_THROW(static_cast<void>(voters.consider(first_pair, 1, 1, start)), pubsub_itc_fw::PreconditionAssertion);
}

TEST(ComponentLeaseVotersTest, ANewlyActiveArbiterGrantsNothingForOneLeasePeriod) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    const Clock::time_point active_at = start + milliseconds{60000};
    voters.became_active(active_at);
    EXPECT_EQ(voters.consider(first_pair, 1, 1, active_at + milliseconds{2999}).verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedWhileRestarting);
    EXPECT_EQ(voters.consider(first_pair, 1, 1, active_at + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
}

TEST(ComponentLeaseVotersTest, EachGroupHasItsOwnVote) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(voters.consider(first_pair, 1, 1, now).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.consider(second_pair, 2, 2, now).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.consider(first_pair, 2, 2, now).verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedPromisedElsewhere);
}

TEST(ComponentLeaseVotersTest, PromisesAreForgottenOnBecomingPassiveAndWaitedOutOnBecomingActiveAgain) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    const Clock::time_point granted_at = start + lease_period;
    EXPECT_EQ(voters.consider(first_pair, 1, 1, granted_at).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    voters.became_passive();
    voters.became_active(granted_at + milliseconds{100});
    // The promise to instance 1 is forgotten, so this arbiter waits a whole period before granting anyone.
    EXPECT_EQ(voters.consider(first_pair, 2, 2, granted_at + milliseconds{200}).verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedWhileRestarting);
}

TEST(ComponentLeaseVotersTest, TheHighestEpochCarriesAcrossAChangeOfActiveArbiter) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.learn_epoch(first_pair, 6);
    voters.became_active(start);
    const pubsub_itc_fw::LeaseVoter::Answer answer = voters.consider(first_pair, 1, 5, start + lease_period);
    EXPECT_EQ(answer.verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedEpochBehind);
    EXPECT_EQ(answer.highest_epoch, 6);
}

TEST(ComponentLeaseVotersTest, AGrantRaisesTheHighestEpoch) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    EXPECT_EQ(voters.consider(first_pair, 2, 6, start + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.highest_epoch(first_pair), 6);
    EXPECT_EQ(voters.highest_epoch(second_pair), 0);
}

TEST(ComponentLeaseVotersTest, AStatementThatAnInstanceMayNotLeadSurvivesAChangeOfActiveArbiter) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    const Clock::time_point granted_at = start + lease_period;
    ASSERT_EQ(voters.consider(first_pair, 1, 1, granted_at).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.record_statement(first_pair, pubsub_itc_fw::LeaderStatement{1, 1, 1, false}), 1);
    // Unlike its promises, the statement must not be forgotten when this arbiter becomes active again.
    voters.became_passive();
    const Clock::time_point active_again = granted_at + milliseconds{60000};
    voters.became_active(active_again);
    EXPECT_EQ(voters.consider(first_pair, 2, 2, active_again + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedMayNotLead);
    EXPECT_EQ(voters.consider(first_pair, 1, 5, active_again + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
}

TEST(ComponentLeaseVotersTest, AStatementLearntFromThePeerArbiterIsHonouredWhenThisOneBecomesActive) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    EXPECT_TRUE(voters.learn_statement(first_pair, pubsub_itc_fw::LeaderStatement{2, 6, 3, false}));
    EXPECT_FALSE(voters.learn_statement(first_pair, pubsub_itc_fw::LeaderStatement{2, 6, 2, true})) << "an older statement replaced a newer one";
    voters.became_active(start);
    EXPECT_EQ(voters.consider(first_pair, 1, 9, start + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::RefusedMayNotLead);
    EXPECT_EQ(voters.consider(second_pair, 1, 9, start + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted)
        << "a statement in one group must not affect another";
}

TEST(ComponentLeaseVotersTest, ALateStatementIsNotEchoed) {
    pubsub_itc_fw::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    ASSERT_EQ(voters.consider(first_pair, 1, 1, start + lease_period).verdict, pubsub_itc_fw::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.record_statement(first_pair, pubsub_itc_fw::LeaderStatement{1, 1, 2, true}), 2);
    EXPECT_EQ(voters.record_statement(first_pair, pubsub_itc_fw::LeaderStatement{1, 1, 1, false}), 0);
    EXPECT_TRUE(voters.statement(first_pair).peer_may_lead);
}
