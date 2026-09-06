// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CatchUpTally.hpp>

namespace pubsub_itc_fw::tests {

// R-0101: a component shall establish that its catch-up was complete before it acts, and shall not
// begin acting if it was not.
//
// Catch-up records are applied silently, so a missing one is invisible in every direction: the
// component logs nothing, produces nothing, and believes it is current. These cases are the
// difference between a catch-up that worked and one that only appeared to.
//
// The stream is filtered by the authority, so the numbers that arrive are a subset of the range
// and the gaps between them are ordinary. What is checked is the count, the order, and that
// nothing repeated.

TEST(CatchUpTallyTest, EverythingSentArrivedInOrder) {
    CatchUpTally tally(100);
    for (int64_t seq = 101; seq <= 105; ++seq) {
        EXPECT_TRUE(tally.offer(seq)) << "seq " << seq << " should have been accepted";
    }
    EXPECT_TRUE(tally.complete_with(5, 105));
    EXPECT_EQ(tally.received(), 5);
    EXPECT_EQ(tally.describe_shortfall(5, 105), "");
}

TEST(CatchUpTallyTest, GapsInTheNumbersAreOrdinaryBecauseTheStreamIsFiltered) {
    // The case that made the contiguous reading wrong. The authority holds 101 through 110 and
    // sends only the four the receiver has any use for; the six it withheld are not missing.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_TRUE(tally.offer(104));
    EXPECT_TRUE(tally.offer(107));
    EXPECT_TRUE(tally.offer(110));
    EXPECT_TRUE(tally.complete_with(4, 110));
    EXPECT_EQ(tally.describe_shortfall(4, 110), "");
}

TEST(CatchUpTallyTest, NothingToCatchUpOnIsCompleteRatherThanSuspicious) {
    // The component was already current. That is the common case after a clean restart and must
    // not read as a failure.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.complete_with(0, 100));
    EXPECT_EQ(tally.received(), 0);
}

TEST(CatchUpTallyTest, AComponentHoldingNothingIsPlacedAtTheHeadAndSentNothing) {
    // A negative position is an instance saying it has applied nothing of this venue's record.
    // The authority answers by placing it at the head rather than replaying history at it.
    CatchUpTally tally(-1);
    EXPECT_TRUE(tally.complete_with(0, 6320));
    EXPECT_EQ(tally.received(), 0);
}

TEST(CatchUpTallyTest, AStreamThatStopsEarlyIsRefused) {
    // The case the requirement exists for. Two of the five sent never arrived, and without this
    // the book would be short of two orders and the engine would act believing itself current.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_TRUE(tally.offer(102));
    EXPECT_TRUE(tally.offer(104));
    EXPECT_FALSE(tally.complete_with(5, 110));

    const std::string why = tally.describe_shortfall(5, 110);
    EXPECT_NE(why.find("5 record(s) were sent"), std::string::npos) << why;
    EXPECT_NE(why.find("3 arrived"), std::string::npos) << why;
    EXPECT_NE(why.find("2 never did"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, ARepeatedRecordIsRefused) {
    // Over a stream read in order from a log this cannot happen, which is exactly why it is
    // worth refusing: if it does happen the stream is not what it claims to be. The count alone
    // would not catch it, because a repeat and a delivery both add one.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_FALSE(tally.offer(101));
    EXPECT_TRUE(tally.offer(102));
    EXPECT_FALSE(tally.complete_with(2, 102)) << "the count agrees, and the stream was still wrong";

    const std::string why = tally.describe_shortfall(2, 102);
    EXPECT_NE(why.find("did not advance"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, ARecordOutOfOrderIsRefused) {
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(105));
    EXPECT_FALSE(tally.offer(103)) << "a record numbered before one already accepted";
    EXPECT_FALSE(tally.complete_with(2, 105));
}

TEST(CatchUpTallyTest, ARecordAtOrBeforeThePresentedPositionIsRefused) {
    // The component said it held everything through 100. Being sent 100 again means the authority
    // answered a different question from the one asked.
    CatchUpTally tally(100);
    EXPECT_FALSE(tally.offer(100));
    EXPECT_FALSE(tally.complete_with(1, 105));
}

TEST(CatchUpTallyTest, AHeadBehindThePresentedPositionIsAContradictionNotASuccess) {
    // The component holds records the authority does not. Nothing is missing from the range,
    // because the range runs backwards -- and a check that only counted would pass it.
    CatchUpTally tally(100);
    EXPECT_FALSE(tally.complete_with(0, 99));

    const std::string why = tally.describe_shortfall(0, 99);
    EXPECT_NE(why.find("behind the position presented"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, MoreArrivingThanWasSentIsRefused) {
    // Duplication in transit, or two authorities answering one request. Either way the receiver
    // cannot say what it holds.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(101));
    EXPECT_TRUE(tally.offer(102));
    EXPECT_TRUE(tally.offer(103));
    EXPECT_FALSE(tally.complete_with(2, 103));

    const std::string why = tally.describe_shortfall(2, 103);
    EXPECT_NE(why.find("more records arrived than were sent"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, TheFirstDiscrepancyIsTheOneReported) {
    // Later records are still offered by the caller, and they must not overwrite the first
    // failure: the first is where the truth stopped and is what a person needs.
    CatchUpTally tally(100);
    EXPECT_TRUE(tally.offer(105));
    EXPECT_FALSE(tally.offer(101));
    EXPECT_FALSE(tally.offer(102));

    const std::string why = tally.describe_shortfall(3, 105);
    EXPECT_NE(why.find("seq_no 101 arrived after seq_no 105"), std::string::npos) << why;
}

TEST(CatchUpTallyTest, ACompleteCatchUpReportsHowMuchItApplied) {
    // "Applied nothing" and "never ran" look identical in a log otherwise.
    CatchUpTally tally(1000);
    for (int64_t seq = 1001; seq <= 1250; ++seq) {
        ASSERT_TRUE(tally.offer(seq));
    }
    EXPECT_TRUE(tally.complete_with(250, 1250));
    EXPECT_EQ(tally.received(), 250);
    EXPECT_EQ(tally.last_accepted(), 1250);
}

} // namespaces
