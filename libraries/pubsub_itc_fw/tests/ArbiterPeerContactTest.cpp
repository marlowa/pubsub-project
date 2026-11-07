// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ArbiterPeerContact.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// A start well after the clock's zero, so that a send time of zero can only mean "none".
const Clock::time_point start = Clock::time_point{} + std::chrono::hours{1};

pubsub_itc_fw::LeaseTiming timing() {
    pubsub_itc_fw::LeaseTiming result;
    result.period = milliseconds{3000};
    result.drift_allowance = milliseconds{250};
    result.renewal_interval = milliseconds{1000};
    return result;
}

} // un-named namespace

TEST(ArbiterPeerContactTest, TheTwoWaitsFollowFromTheLeaseTimings) {
    const pubsub_itc_fw::ArbiterPeerContact contact(timing(), start);
    EXPECT_EQ(contact.may_be_behind_after(), milliseconds{7000});
    EXPECT_EQ(contact.confirms_alone_after(), milliseconds{10000});
}

TEST(ArbiterPeerContactTest, AnArbiterConfirmsAloneOnlyOnceItHasHeardNothingFromTheOtherForLongEnough) {
    pubsub_itc_fw::ArbiterPeerContact contact(timing(), start);
    EXPECT_FALSE(contact.may_confirm_alone(start + milliseconds{9999})) << "starting counts as having heard from the other arbiter";
    EXPECT_TRUE(contact.may_confirm_alone(start + milliseconds{10000}));

    contact.heard(start + milliseconds{10000});
    EXPECT_FALSE(contact.may_confirm_alone(start + milliseconds{10000})) << "a message from the other arbiter ends confirming alone";
    EXPECT_FALSE(contact.may_confirm_alone(start + milliseconds{19999}));
    EXPECT_TRUE(contact.may_confirm_alone(start + milliseconds{20000}));
}

TEST(ArbiterPeerContactTest, AnArbiterThatHasNeverHadItsHeartbeatEchoedMayBeBehind) {
    const pubsub_itc_fw::ArbiterPeerContact contact(timing(), start);
    EXPECT_TRUE(contact.may_be_behind(start));
    EXPECT_TRUE(contact.may_be_behind(start + milliseconds{100}));
}

TEST(ArbiterPeerContactTest, AnArbiterIsNotBehindWhileTheEchoOfItsOwnHeartbeatIsRecent) {
    pubsub_itc_fw::ArbiterPeerContact contact(timing(), start);
    const Clock::time_point sent = start + milliseconds{500};
    contact.on_heartbeat(pubsub_itc_fw::ArbiterPeerContact::sent_at(start + milliseconds{400}), pubsub_itc_fw::ArbiterPeerContact::sent_at(sent),
                         start + milliseconds{600});
    EXPECT_FALSE(contact.may_be_behind(sent + milliseconds{6999}));
    EXPECT_TRUE(contact.may_be_behind(sent + milliseconds{7000})) << "the other may since have confirmed alone a statement this one lacks";

    // An older echo, arriving late, does not make the arbiter think it is less behind than the newest echo says.
    contact.on_heartbeat(0, pubsub_itc_fw::ArbiterPeerContact::sent_at(start + milliseconds{100}), start + milliseconds{700});
    EXPECT_FALSE(contact.may_be_behind(sent + milliseconds{6999}));
}

TEST(ArbiterPeerContactTest, TheEchoIsTheNewestSendTimeReceived) {
    pubsub_itc_fw::ArbiterPeerContact contact(timing(), start);
    EXPECT_EQ(contact.echo(), 0);
    contact.on_heartbeat(500, 0, start);
    contact.on_heartbeat(300, 0, start);
    EXPECT_EQ(contact.echo(), 500) << "a late heartbeat must not move the echo back";
}

TEST(ArbiterPeerContactTest, AMachineDeathLeavesTheSurvivorNotBehindAndLaterAbleToConfirmAlone) {
    // The active arbiter's machine dies just after echoing the survivor's heartbeat. The survivor becomes active once its
    // promise to the dead arbiter has run out and the witness grants it, about one lease period and one renewal interval
    // later, and must then grant as usual. Later it confirms alone.
    pubsub_itc_fw::ArbiterPeerContact survivor(timing(), start);
    const Clock::time_point last_heartbeat_sent = start + milliseconds{5000};
    survivor.on_heartbeat(1, pubsub_itc_fw::ArbiterPeerContact::sent_at(last_heartbeat_sent), last_heartbeat_sent + milliseconds{1});
    const Clock::time_point becomes_active = last_heartbeat_sent + milliseconds{3000 + 1000 + 200};
    EXPECT_FALSE(survivor.may_be_behind(becomes_active));
    EXPECT_FALSE(survivor.may_confirm_alone(becomes_active));
    EXPECT_TRUE(survivor.may_confirm_alone(last_heartbeat_sent + milliseconds{1} + milliseconds{10000}));
}
