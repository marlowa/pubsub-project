// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <leader_follower.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>

#include "EngineOrderRouting.hpp"

namespace {

using sequencer::ClaimIfNothingRoutedFlag;
using sequencer::EngineOrderRouting;
using Outcome = EngineOrderRouting::AnnouncementOutcome;
using pubsub_itc_fw_app::Role;

constexpr int64_t primary = 1;
constexpr int64_t secondary = 2;
const ClaimIfNothingRoutedFlag claim{ClaimIfNothingRoutedFlag::ClaimIfNothingRouted};
const ClaimIfNothingRoutedFlag do_not_claim{ClaimIfNothingRoutedFlag::DoNotClaimIfNothingRouted};
const pubsub_itc_fw::ConnectionID connection_5{5};
const pubsub_itc_fw::ConnectionID connection_6{6};
const pubsub_itc_fw::ConnectionID connection_7{7};

// The sequence of events in BUG-0108, as the secondary sequencer's log recorded it while it followed.
// The engine on connection 5 must still be recognised when it asks to be brought up to date, and must
// then carry orders. Before the fix the request on connection 5 was dropped, and the venue had no
// matching engine while one was leading and asking.
TEST(EngineOrderRoutingTest, AFollowingSequencerDoesNotForgetAnEngineThatLaterLeads) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.active(), connection_5);
    EXPECT_EQ(routing.standby(), connection_6);

    // The secondary engine asks to catch up while this sequencer follows, and is routed to.
    routing.asked_to_catch_up_while_following(connection_6);
    EXPECT_EQ(routing.active(), connection_6);
    EXPECT_EQ(routing.standby(), connection_5) << "the primary engine's connection was forgotten";

    // Then it announces that it follows, and no leader is known.
    EXPECT_EQ(routing.announced(secondary, Role::follower, 18), Outcome::FollowerWithdrawnNoLeader);
    EXPECT_FALSE(routing.active().is_valid());

    // The primary engine leads and asks, on connection 5, to be brought up to date.
    EXPECT_TRUE(routing.is_engine_connection(connection_5)) << "the request to catch up would be dropped";
    routing.caught_up_to_lead(connection_5);
    EXPECT_EQ(routing.active(), connection_5);
}

// An engine that says it follows while it carries orders hands them to the engine known to lead,
// rather than leaving orders with nowhere to go beside a healthy leader (BUG-0077).
TEST(EngineOrderRoutingTest, AFollowerCarryingOrdersHandsThemToTheKnownLeader) {
    EngineOrderRouting routing;
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.announced(secondary, Role::leader, 4), Outcome::RoutedToLeader);
    routing.connected(primary, connection_5, claim);
    EXPECT_EQ(routing.active(), connection_6) << "a primary that connects must not take orders from a leader";

    routing.asked_to_catch_up_while_following(connection_5);
    ASSERT_EQ(routing.active(), connection_5);
    EXPECT_EQ(routing.announced(primary, Role::follower, 5), Outcome::FollowerHandedToLeader);
    EXPECT_EQ(routing.active(), connection_6);
}

// A restarted engine announces on a connection it opens, and may do so before this sequencer has
// reopened its order connection to it. Once that connection opens, it carries orders.
TEST(EngineOrderRoutingTest, AnEngineThatAnnouncedLeadershipBeforeItsConnectionOpenedGetsTheOrders) {
    EngineOrderRouting routing;
    EXPECT_EQ(routing.announced(secondary, Role::leader, 9), Outcome::NoOrderConnection);
    routing.connected(secondary, connection_7, do_not_claim);
    EXPECT_EQ(routing.active(), connection_7);
}

// An instance whose leadership has been superseded may still announce that it leads; its epoch gives it away.
TEST(EngineOrderRoutingTest, AnAnnouncementBehindTheAcceptedEpochIsRefused) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.announced(secondary, Role::leader, 12), Outcome::RoutedToLeader);
    EXPECT_EQ(routing.announced(primary, Role::leader, 11), Outcome::RefusedAsBehind);
    EXPECT_EQ(routing.active(), connection_6);
    EXPECT_EQ(routing.announced_epoch(), 12);
}

// A connection that closes is forgotten: it carries nothing, and a request on it is not recognised.
TEST(EngineOrderRoutingTest, ALostConnectionIsForgottenAndCarriesNothing) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    routing.lost(connection_5);
    EXPECT_FALSE(routing.active().is_valid());
    EXPECT_FALSE(routing.is_engine_connection(connection_5));
    EXPECT_TRUE(routing.is_engine_connection(connection_6));
    EXPECT_EQ(routing.standby(), connection_6);
}

} // un-named namespace
