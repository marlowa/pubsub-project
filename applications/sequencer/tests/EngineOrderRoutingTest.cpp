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

// The primary's connection carries orders when nothing else does, and an engine that asks to be brought up to date while
// this sequencer follows is routed to, but neither is known to lead, so neither is sent orders: they are held until an
// engine is (docs/availability/voided_commands.md, section 5.1).
TEST(EngineOrderRoutingTest, OnlyAnEngineThatAnnouncedOrWasCaughtUpToLeadIsKnownToLead) {
    EngineOrderRouting routing;
    EXPECT_FALSE(routing.active_is_known_to_lead());
    routing.connected(primary, connection_5, claim);
    ASSERT_EQ(routing.active(), connection_5);
    EXPECT_FALSE(routing.active_is_known_to_lead()) << "a primary that has only connected is not known to lead";

    routing.caught_up_to_lead(connection_5);
    EXPECT_TRUE(routing.active_is_known_to_lead());

    routing.connected(secondary, connection_6, do_not_claim);
    routing.asked_to_catch_up_while_following(connection_6);
    ASSERT_EQ(routing.active(), connection_6);
    EXPECT_FALSE(routing.active_is_known_to_lead()) << "an engine asking to be brought up to date is not yet leading";

    EXPECT_EQ(routing.announced(secondary, Role::leader, 3), Outcome::AlreadyRoutedToLeader);
    EXPECT_TRUE(routing.active_is_known_to_lead());

    EXPECT_EQ(routing.announced(secondary, Role::follower, 4), Outcome::FollowerWithdrawnNoLeader);
    EXPECT_FALSE(routing.active_is_known_to_lead());

    EXPECT_EQ(routing.announced(primary, Role::leader, 5), Outcome::RoutedToLeader);
    EXPECT_TRUE(routing.active_is_known_to_lead());
    routing.lost(connection_5);
    EXPECT_FALSE(routing.active_is_known_to_lead()) << "a lost connection carries nothing";
}

// An engine that led is killed: the connection it announced on is lost, and so is its order connection. Restarted, it
// opens a new order connection, which must not be taken to lead on the dead process's announcement: sent orders live at
// once, it would be sent them again in the catch-up it then asks for, marked as possible repeats.
TEST(EngineOrderRoutingTest, AnAnnouncementWhoseConnectionIsLostIsForgotten) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    EXPECT_EQ(routing.announced(primary, Role::leader, 3), Outcome::AlreadyRoutedToLeader);
    ASSERT_TRUE(routing.active_is_known_to_lead());

    routing.leader_announcement_lost(primary);
    EXPECT_FALSE(routing.active_is_known_to_lead()) << "the process that announced may be dead";
    EXPECT_EQ(routing.active(), connection_5) << "orders stay routed where they were";
    routing.lost(connection_5);

    routing.connected(primary, connection_7, claim);
    EXPECT_EQ(routing.active(), connection_7);
    EXPECT_FALSE(routing.active_is_known_to_lead()) << "a restarted process is taken to lead on its predecessor's announcement";
    routing.caught_up_to_lead(connection_7);
    EXPECT_TRUE(routing.active_is_known_to_lead());
}

// An engine that announced it leads before its order connection opened is known to lead once it opens.
TEST(EngineOrderRoutingTest, AnEngineThatAnnouncedBeforeItsConnectionOpenedIsKnownToLead) {
    EngineOrderRouting routing;
    EXPECT_EQ(routing.announced(secondary, Role::leader, 9), Outcome::NoOrderConnection);
    routing.connected(secondary, connection_7, do_not_claim);
    EXPECT_TRUE(routing.active_is_known_to_lead());
}

// An engine whose lease ran out while it was being brought up to date in order to lead announces that it follows, and
// then asks to be brought up to date in order to follow. The hand-over of its earlier catch-up can be acted on between
// the two, routing orders to it as an engine that leads. Its request to follow corrects that: the orders go to the
// engine known to lead, or nowhere.
TEST(EngineOrderRoutingTest, AnEngineThatAsksToFollowIsNotKnownToLead) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.announced(primary, Role::follower, 7), Outcome::FollowerWithdrawnNoLeader);

    // The hand-over of the catch-up to lead it asked for before its lease ran out.
    routing.caught_up_to_lead(connection_5);
    ASSERT_TRUE(routing.active_is_known_to_lead());

    EXPECT_EQ(routing.asked_to_follow(connection_5), Outcome::FollowerWithdrawnNoLeader);
    EXPECT_FALSE(routing.active().is_valid());
    EXPECT_FALSE(routing.active_is_known_to_lead());

    // The other engine then leads, and the orders go to it.
    EXPECT_EQ(routing.announced(secondary, Role::leader, 8), Outcome::RoutedToLeader);
    EXPECT_EQ(routing.active(), connection_6);
}

TEST(EngineOrderRoutingTest, AnEngineThatAsksToFollowHandsItsOrdersToTheKnownLeader) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.announced(secondary, Role::leader, 8), Outcome::RoutedToLeader);
    routing.caught_up_to_lead(connection_5);

    EXPECT_EQ(routing.asked_to_follow(connection_5), Outcome::FollowerHandedToLeader);
    EXPECT_EQ(routing.active(), connection_6);
    EXPECT_TRUE(routing.active_is_known_to_lead());
}

TEST(EngineOrderRoutingTest, AnEngineThatAsksToFollowAndCarriesNoOrdersChangesNothing) {
    EngineOrderRouting routing;
    routing.connected(primary, connection_5, claim);
    routing.connected(secondary, connection_6, do_not_claim);
    EXPECT_EQ(routing.announced(primary, Role::leader, 8), Outcome::AlreadyRoutedToLeader);

    EXPECT_EQ(routing.asked_to_follow(connection_6), Outcome::FollowerNotCarryingOrders);
    EXPECT_EQ(routing.active(), connection_5);
    EXPECT_TRUE(routing.active_is_known_to_lead());
    EXPECT_EQ(routing.asked_to_follow(connection_7), Outcome::NoOrderConnection) << "no engine is on connection 7";
}

} // un-named namespace
