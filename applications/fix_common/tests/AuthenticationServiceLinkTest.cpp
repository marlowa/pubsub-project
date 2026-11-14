// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "AuthenticationServiceLink.hpp"

namespace fix_common::tests {

namespace {

using Clock = AuthenticationServiceLink::Clock;
using HeartbeatAction = AuthenticationServiceLink::HeartbeatAction;
using LossCause = AuthenticationServiceLink::LossCause;
using State = AuthenticationServiceLink::State;
using std::chrono::milliseconds;
using std::chrono::seconds;

/// An arbitrary starting time, well clear of the clock's epoch.
const Clock::time_point start_time = Clock::time_point{} + std::chrono::hours(1000);

const pubsub_itc_fw::ConnectionID first_connection{7, std::string("authentication_service_primary")};
const pubsub_itc_fw::ConnectionID second_connection{8, std::string("authentication_service_primary")};
const pubsub_itc_fw::ConnectionID some_other_connection{9, std::string("sequencer_primary")};

/// A link with a heartbeat every 30 seconds, as configured in every environment.
AuthenticationServiceLink a_link() {
    return AuthenticationServiceLink(seconds(30));
}

/// A link whose connection was made at start_time and answered at once.
AuthenticationServiceLink an_answering_link() {
    AuthenticationServiceLink link = a_link();
    link.on_established(first_connection, start_time);
    EXPECT_TRUE(link.on_heard(start_time + milliseconds(1)));
    return link;
}

} // namespaces

TEST(AuthenticationServiceLinkTest, ANewLinkHasNoConnectionAndIsNotUsable) {
    AuthenticationServiceLink link = a_link();
    EXPECT_EQ(link.state(), State::NotConnected);
    EXPECT_FALSE(link.is_usable());
    EXPECT_FALSE(link.owns(first_connection));
    EXPECT_FALSE(link.connection_id().is_valid());
}

TEST(AuthenticationServiceLinkTest, AHeartbeatIntervalOfZeroIsRefused) {
    EXPECT_THROW(AuthenticationServiceLink(milliseconds(0)), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(AuthenticationServiceLink(milliseconds(-1)), pubsub_itc_fw::PreconditionAssertion);
}

TEST(AuthenticationServiceLinkTest, TheSilenceLimitIsThreeHeartbeatIntervals) {
    EXPECT_EQ(a_link().silence_limit(), seconds(90));
}

TEST(AuthenticationServiceLinkTest, ANewConnectionIsNotUsableUntilSomethingArrivesOnIt) {
    AuthenticationServiceLink link = a_link();
    link.on_established(first_connection, start_time);
    EXPECT_EQ(link.state(), State::WaitingForFirstAnswer);
    EXPECT_TRUE(link.owns(first_connection));
    EXPECT_FALSE(link.is_usable());

    EXPECT_TRUE(link.on_heard(start_time + milliseconds(1))) << "the first thing to arrive makes the connection usable, and says so";
    EXPECT_EQ(link.state(), State::Answering);
    EXPECT_TRUE(link.is_usable());
}

TEST(AuthenticationServiceLinkTest, OnlyTheFirstThingToArriveSaysTheConnectionBecameUsable) {
    AuthenticationServiceLink link = an_answering_link();
    EXPECT_FALSE(link.on_heard(start_time + seconds(30)));
    EXPECT_FALSE(link.on_heard(start_time + seconds(60)));
    EXPECT_TRUE(link.is_usable());
}

TEST(AuthenticationServiceLinkTest, HearingWithNoConnectionIsRefused) {
    AuthenticationServiceLink link = a_link();
    EXPECT_THROW(static_cast<void>(link.on_heard(start_time)), pubsub_itc_fw::PreconditionAssertion);
}

TEST(AuthenticationServiceLinkTest, OwnsOnlyItsOwnConnection) {
    AuthenticationServiceLink link = an_answering_link();
    EXPECT_TRUE(link.owns(first_connection));
    EXPECT_FALSE(link.owns(second_connection));
    EXPECT_FALSE(link.owns(some_other_connection));
}

TEST(AuthenticationServiceLinkTest, WithNoConnectionTheHeartbeatTimerDoesNothing) {
    AuthenticationServiceLink link = a_link();
    EXPECT_EQ(link.on_heartbeat_timer(start_time), HeartbeatAction::Nothing);
    EXPECT_EQ(link.on_heartbeat_timer(start_time + seconds(1000)), HeartbeatAction::Nothing);
}

TEST(AuthenticationServiceLinkTest, AnAnsweringConnectionIsSentAHeartbeatEachTime) {
    AuthenticationServiceLink link = an_answering_link();
    for (int beat = 1; beat <= 10; ++beat) {
        const Clock::time_point now = start_time + seconds(30 * beat);
        EXPECT_EQ(link.on_heartbeat_timer(now), HeartbeatAction::SendHeartbeat) << "heartbeat " << beat;
        static_cast<void>(link.on_heard(now + milliseconds(1)));
    }
    EXPECT_TRUE(link.is_usable());
}

TEST(AuthenticationServiceLinkTest, ANewConnectionWaitingForItsFirstAnswerIsSentHeartbeats) {
    AuthenticationServiceLink link = a_link();
    link.on_established(first_connection, start_time);
    EXPECT_EQ(link.on_heartbeat_timer(start_time + seconds(30)), HeartbeatAction::SendHeartbeat);
}

TEST(AuthenticationServiceLinkTest, ExactlyTheSilenceLimitIsNotYetSilent) {
    AuthenticationServiceLink link = an_answering_link();
    const Clock::time_point last_heard = link.last_heard();
    EXPECT_EQ(link.on_heartbeat_timer(last_heard + seconds(90)), HeartbeatAction::SendHeartbeat);
    EXPECT_TRUE(link.is_usable());
}

TEST(AuthenticationServiceLinkTest, NothingHeardForLongerThanTheSilenceLimitClosesTheConnection) {
    AuthenticationServiceLink link = an_answering_link();
    const Clock::time_point last_heard = link.last_heard();
    EXPECT_EQ(link.on_heartbeat_timer(last_heard + seconds(30)), HeartbeatAction::SendHeartbeat);
    EXPECT_EQ(link.on_heartbeat_timer(last_heard + seconds(60)), HeartbeatAction::SendHeartbeat);
    EXPECT_EQ(link.on_heartbeat_timer(last_heard + seconds(90)), HeartbeatAction::SendHeartbeat);
    EXPECT_EQ(link.on_heartbeat_timer(last_heard + seconds(90) + milliseconds(1)), HeartbeatAction::CloseAsSilent);
    EXPECT_EQ(link.state(), State::BeingClosed);
    EXPECT_FALSE(link.is_usable()) << "a connection being closed must not be given logons";
}

TEST(AuthenticationServiceLinkTest, ANewConnectionThatNeverAnswersIsClosedAsSilent) {
    AuthenticationServiceLink link = a_link();
    link.on_established(first_connection, start_time);
    EXPECT_EQ(link.on_heartbeat_timer(start_time + seconds(91)), HeartbeatAction::CloseAsSilent);
}

TEST(AuthenticationServiceLinkTest, AConnectionBeingClosedIsSentNothingMore) {
    AuthenticationServiceLink link = an_answering_link();
    ASSERT_EQ(link.on_heartbeat_timer(start_time + seconds(120)), HeartbeatAction::CloseAsSilent);
    EXPECT_EQ(link.on_heartbeat_timer(start_time + seconds(150)), HeartbeatAction::Nothing);
    EXPECT_EQ(link.on_heartbeat_timer(start_time + seconds(180)), HeartbeatAction::Nothing);
}

TEST(AuthenticationServiceLinkTest, AnswersArrivingKeepTheConnectionOpenHoweverLongItRuns) {
    AuthenticationServiceLink link = an_answering_link();
    // An answer that arrives just inside each silence limit is enough, even if most heartbeats are never answered.
    for (int beat = 1; beat <= 100; ++beat) {
        const Clock::time_point now = start_time + seconds(30 * beat);
        EXPECT_EQ(link.on_heartbeat_timer(now), HeartbeatAction::SendHeartbeat) << "heartbeat " << beat;
        if (beat % 3 == 0) {
            static_cast<void>(link.on_heard(now));
        }
    }
}

TEST(AuthenticationServiceLinkTest, ALossThePeerCausedIsReportedAsSuch) {
    AuthenticationServiceLink link = an_answering_link();
    EXPECT_EQ(link.on_lost(first_connection), LossCause::ClosedByPeer);
    EXPECT_EQ(link.state(), State::NotConnected);
    EXPECT_FALSE(link.is_usable());
    EXPECT_FALSE(link.owns(first_connection));
}

TEST(AuthenticationServiceLinkTest, ALossAfterClosingForSilenceIsReportedAsSuch) {
    AuthenticationServiceLink link = an_answering_link();
    ASSERT_EQ(link.on_heartbeat_timer(start_time + seconds(120)), HeartbeatAction::CloseAsSilent);
    EXPECT_EQ(link.on_lost(first_connection), LossCause::ClosedForSilence);
    EXPECT_EQ(link.state(), State::NotConnected);
}

TEST(AuthenticationServiceLinkTest, TheLossOfAnotherConnectionLeavesTheLinkAlone) {
    AuthenticationServiceLink link = an_answering_link();
    EXPECT_EQ(link.on_lost(some_other_connection), LossCause::NotThisConnection);
    EXPECT_EQ(link.on_lost(second_connection), LossCause::NotThisConnection);
    EXPECT_TRUE(link.is_usable());
    EXPECT_TRUE(link.owns(first_connection));
}

TEST(AuthenticationServiceLinkTest, ALinkWithNoConnectionReportsNoLoss) {
    AuthenticationServiceLink link = a_link();
    EXPECT_EQ(link.on_lost(first_connection), LossCause::NotThisConnection);
}

TEST(AuthenticationServiceLinkTest, AReconnectionStartsAgainWaitingForTheFirstAnswer) {
    AuthenticationServiceLink link = an_answering_link();
    ASSERT_EQ(link.on_heartbeat_timer(start_time + seconds(120)), HeartbeatAction::CloseAsSilent);
    ASSERT_EQ(link.on_lost(first_connection), LossCause::ClosedForSilence);

    const Clock::time_point reconnected = start_time + seconds(122);
    link.on_established(second_connection, reconnected);
    EXPECT_EQ(link.state(), State::WaitingForFirstAnswer);
    EXPECT_FALSE(link.is_usable()) << "a process that is stopped still accepts connections, so a new one proves nothing";
    EXPECT_TRUE(link.owns(second_connection));
    EXPECT_FALSE(link.owns(first_connection));
    EXPECT_EQ(link.on_heartbeat_timer(reconnected + seconds(30)), HeartbeatAction::SendHeartbeat) << "the silence limit counts from the new connection";
}

} // namespaces
