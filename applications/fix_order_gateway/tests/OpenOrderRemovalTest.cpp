// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for the rule deciding which resting order an ExecutionReport retires.
//
// What is protected here is a leak that was silent for as long as a session lasted, and a
// correctness failure behind it. Every cancelled order stayed in the gateway's open-order
// map, and cancel-on-disconnect walks that map: stale entries mean the venue emits cancels
// for orders that were retired long ago, at the moment a member has just dropped.
//
// It was invisible because it needed two things at once. Cancels never matched, and the
// matching engine never fills anything, so the one terminal report that reaches a gateway in
// practice was the one looked up wrongly. Either alone would have shown up.

#include <gtest/gtest.h>

#include "OpenOrderRemoval.hpp"

namespace {

using pubsub_itc_fw_app::OrdStatus;

open_orders::OpenOrderRemoval decide(OrdStatus status, std::string_view cl_ord_id, bool has_orig, std::string_view orig_cl_ord_id) {
    return open_orders::decide_open_order_removal(status, !cl_ord_id.empty(), cl_ord_id, has_orig, orig_cl_ord_id);
}

TEST(OpenOrderRemovalTest, ANonTerminalReportRemovesNothing) {
    // A New or PartiallyFilled report means the order is still resting.
    const auto removal = decide(OrdStatus::New, "ORD-1", false, {});
    EXPECT_FALSE(removal.remove);
}

TEST(OpenOrderRemovalTest, AFillRemovesTheOrderItNames) {
    // A fill carries only ClOrdID, and that is the resting order.
    const auto removal = decide(OrdStatus::Filled, "ORD-1", false, {});
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-1");
}

TEST(OpenOrderRemovalTest, AnAcceptedCancelRemovesTheOrderNamedInOrigClOrdId) {
    // The defect this whole file exists for. ClOrdID is the cancel request, minted for the
    // request and never filed; OrigClOrdID is the order on the book.
    const auto removal = decide(OrdStatus::Canceled, "CANCEL-9", true, "ORD-1");
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-1") << "removing by ClOrdID searches for something never filed";
}

TEST(OpenOrderRemovalTest, ARejectedCancelRemovesNothing) {
    // The engine refused the cancel, so the order it named is still resting. Removing would
    // leave a live order with no cancel-on-disconnect cover, which is the worse error.
    const auto removal = decide(OrdStatus::Rejected, "CANCEL-9", true, "ORD-1");
    EXPECT_FALSE(removal.remove);
}

TEST(OpenOrderRemovalTest, ARejectedOrderRemovesItself) {
    // A rejection with no OrigClOrdID rejects the order itself, not a cancel of one, so the
    // order named in ClOrdID is the one to drop. This is why the rule cannot simply be
    // "never remove on Rejected".
    const auto removal = decide(OrdStatus::Rejected, "ORD-1", false, {});
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-1");
}

TEST(OpenOrderRemovalTest, AnExpiryRemovesTheOrderNamedInOrigClOrdId) {
    // A GoodTillDate order reaching its expiry is retired by the venue rather than by a
    // request, but the report still names the original order.
    const auto removal = decide(OrdStatus::Expired, "EXP-3", true, "ORD-7");
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-7");
}

TEST(OpenOrderRemovalTest, DoneForDayRemovesTheOrderItNames) {
    const auto removal = decide(OrdStatus::DoneForDay, "ORD-4", false, {});
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-4");
}

TEST(OpenOrderRemovalTest, AnEmptyOrigClOrdIdFallsBackToClOrdId) {
    // Present but empty is not a reference to another order. Treating it as one would look
    // up the empty key and remove nothing.
    const auto removal = decide(OrdStatus::Canceled, "ORD-1", true, "");
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key, "ORD-1");
}

TEST(OpenOrderRemovalTest, AReportNamingNothingRemovesNothing) {
    // No ClOrdID and no OrigClOrdID. Guessing would unfile a live order.
    const auto removal = decide(OrdStatus::Canceled, "", false, {});
    EXPECT_FALSE(removal.remove);
}

TEST(OpenOrderRemovalTest, TheKeyIsNotCopied) {
    // The caller looks the key up as a string_view over the report's payload, so it has to
    // point at the caller's own bytes rather than at anything this function owns.
    const std::string orig = "ORD-42";
    const auto removal = decide(OrdStatus::Canceled, "CANCEL-1", true, orig);
    ASSERT_TRUE(removal.remove);
    EXPECT_EQ(removal.key.data(), orig.data());
}

} // namespaces
