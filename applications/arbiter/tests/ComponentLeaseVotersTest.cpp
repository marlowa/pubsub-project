// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <gtest/gtest.h>

#include "ComponentLeaseVoters.hpp"

#include <LeaseVoter.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds lease_period{3000};
const Clock::time_point start{};
constexpr auto sequencer = pubsub_itc_fw_app::ComponentGroup::sequencer;
constexpr auto matching_engine = pubsub_itc_fw_app::ComponentGroup::matching_engine;

} // un-named namespace

TEST(ComponentLeaseVotersTest, APassiveArbiterMayNotAnswer) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    EXPECT_THROW(static_cast<void>(voters.consider(sequencer, 1, 1, start)), pubsub_itc_fw::PreconditionAssertion);
}

TEST(ComponentLeaseVotersTest, ANewlyActiveArbiterGrantsNothingForOneLeasePeriod) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    const Clock::time_point active_at = start + milliseconds{60000};
    voters.became_active(active_at);
    EXPECT_EQ(voters.consider(sequencer, 1, 1, active_at + milliseconds{2999}).verdict, fix_common::LeaseVoter::Verdict::RefusedWhileRestarting);
    EXPECT_EQ(voters.consider(sequencer, 1, 1, active_at + lease_period).verdict, fix_common::LeaseVoter::Verdict::Granted);
}

TEST(ComponentLeaseVotersTest, EachGroupHasItsOwnVote) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    const Clock::time_point now = start + lease_period;
    EXPECT_EQ(voters.consider(sequencer, 1, 1, now).verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.consider(matching_engine, 2, 2, now).verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.consider(sequencer, 2, 2, now).verdict, fix_common::LeaseVoter::Verdict::RefusedPromisedElsewhere);
}

TEST(ComponentLeaseVotersTest, PromisesAreForgottenOnBecomingPassiveAndWaitedOutOnBecomingActiveAgain) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    const Clock::time_point granted_at = start + lease_period;
    EXPECT_EQ(voters.consider(sequencer, 1, 1, granted_at).verdict, fix_common::LeaseVoter::Verdict::Granted);
    voters.became_passive();
    voters.became_active(granted_at + milliseconds{100});
    // The promise to instance 1 is forgotten, so this arbiter waits a whole period before granting anyone.
    EXPECT_EQ(voters.consider(sequencer, 2, 2, granted_at + milliseconds{200}).verdict, fix_common::LeaseVoter::Verdict::RefusedWhileRestarting);
}

TEST(ComponentLeaseVotersTest, TheHighestEpochCarriesAcrossAChangeOfActiveArbiter) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    voters.learn_epoch(sequencer, 6);
    voters.became_active(start);
    const fix_common::LeaseVoter::Answer answer = voters.consider(sequencer, 1, 5, start + lease_period);
    EXPECT_EQ(answer.verdict, fix_common::LeaseVoter::Verdict::RefusedEpochBehind);
    EXPECT_EQ(answer.highest_epoch, 6);
}

TEST(ComponentLeaseVotersTest, AGrantRaisesTheHighestEpoch) {
    arbiter::ComponentLeaseVoters voters(lease_period);
    voters.became_active(start);
    EXPECT_EQ(voters.consider(sequencer, 2, 6, start + lease_period).verdict, fix_common::LeaseVoter::Verdict::Granted);
    EXPECT_EQ(voters.highest_epoch(sequencer), 6);
    EXPECT_EQ(voters.highest_epoch(matching_engine), 0);
}
