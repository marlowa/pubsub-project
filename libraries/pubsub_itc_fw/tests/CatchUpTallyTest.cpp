// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CatchUpTally.hpp>

namespace pubsub_itc_fw::tests {

// R-0101: a component shall establish that it received every record between the position it
// presented and the position it reached, and shall not begin acting if it did not.
//
// Catch-up records are applied silently, so a missing one is invisible in every direction: the
// component logs nothing, produces nothing, and believes it is current. These cases are the
// difference between a catch-up that worked and one that only appeared to.

TEST(CatchUpTallyTest, AContiguousRunFromThePresentedPositionIsComplete) {
    CatchUpTally tally(100);
    for (int64_t seq = 101; seq <= 105; ++seq) {
        EXPECT_TRUE(tally.offer(seq)) << "seq " << seq << " should have been the one expected";
    }
    EXPECT_TRUE(tally.complete_through(105));
    EXPECT_EQ(tally.received(), 5);
    EXPECT_EQ(tally.describe_shortfall(105), "");
}

TEST(CatchUpTallyTest, NothingToCatchUpOnIsCompleteRatherThanSuspicious) {
    // The component was already current. That is the common case after a clean restart and must
    // not read as a failure.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.complete_through(100));
    EXPECT_EQ(tally.received(), 0);
}

TEST(CatchUpTallyTest, AHoldingOfNothingStartsAtOne) {
    CatchUpTally tally(0);
    EXPECT_EQ(tally.expected_next(), 1);
    EXPECT_TRUE(tally.offer(1));
    EXPECT_TRUE(tally.complete_through(1));
}

TEST(CatchUpTallyTest, ARecordMissingFromTheMiddleIsRefused) {
    // The case the requirement exists for. 103 is never delivered, and without this check the
    // book would be missing an order and the engine would promote believing itself current.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_TRUE(tally.offer(102));
    EXPECT_FALSE(tally.offer(104)) << "a jump from 102 to 104 must not be accepted";
    EXPECT_FALSE(tally.complete_through(105));

    const std::string why = tally.describe_shortfall(105);
    EXPECT_NE(why.find("expected seq_no 103"), std::string::npos) << why;
    EXPECT_NE(why.find("received 104"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, AStreamThatStopsEarlyIsRefused) {
    // Nothing was out of order; the stream simply ended before the authority's head. A tally
    // that only checked for jumps would call this a success.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_TRUE(tally.offer(102));
    EXPECT_FALSE(tally.complete_through(105));

    const std::string why = tally.describe_shortfall(105);
    EXPECT_NE(why.find("stopped early"), std::string::npos) << why;
    EXPECT_NE(why.find("103"), std::string::npos) << why;
    EXPECT_NE(why.find("105"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, ARepeatedRecordIsRefused) {
    // Over a stream read in order from a log this cannot happen, which is exactly why it is
    // worth refusing: if it does happen the stream is not what it claims to be.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_FALSE(tally.offer(101));
    EXPECT_FALSE(tally.complete_through(101));
}

TEST(CatchUpTallyTest, AHeadBehindThePresentedPositionIsAContradictionNotASuccess) {
    // The component holds records the authority does not. Nothing is missing from the range,
    // because the range runs backwards -- and a check that only counted would pass it.
    CatchUpTally tally(100);
    EXPECT_FALSE(tally.complete_through(99));

    const std::string why = tally.describe_shortfall(99);
    EXPECT_NE(why.find("behind the position presented"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, TheFirstDiscrepancyIsTheOneReported) {
    // Later records are still offered by the caller, and they must not overwrite the first
    // failure: the first is where the truth stopped and is what a person needs.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_FALSE(tally.offer(150));
    EXPECT_FALSE(tally.offer(151));
    EXPECT_FALSE(tally.offer(152));

    const std::string why = tally.describe_shortfall(152);
    EXPECT_NE(why.find("expected seq_no 102"), std::string::npos) << why;
    EXPECT_NE(why.find("received 150"), std::string::npos) << why;
    EXPECT_NE(why.find("48 record(s)"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, ACompleteCatchUpReportsHowMuchItApplied) {
    // "Applied nothing" and "never ran" look identical in a log otherwise.
    CatchUpTally tally(1000);
    for (int64_t seq = 1001; seq <= 1250; ++seq) {
        ASSERT_TRUE(tally.offer(seq));
    }
    EXPECT_TRUE(tally.complete_through(1250));
    EXPECT_EQ(tally.received(), 250);
    EXPECT_EQ(tally.expected_next(), 1251);
}

} // namespaces
