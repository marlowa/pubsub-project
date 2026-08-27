// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for the record of which outbound sequence numbers held an execution report.
//
// What is protected here is the fact a resend is built on. A number this record covers is
// replayed onto; a number it does not cover is gap-filled. Get the second case wrong in the
// permissive direction and the venue replays a report onto a number that held a heartbeat,
// which puts more messages into a range than it holds numbers -- the member counts every
// message it is handed, so the two sides come out of the resend disagreeing and the session
// dies on the venue's next message. That was BUG-0051.
//
// The end-to-end scenarios (ha_test.py 22, 23 and 40) exercise the common shapes. What they
// cannot reach is the arithmetic at the edges: ranges that abut, ranges that overlap, a query
// that straddles a boundary, and what trimming leaves behind. Those are here.

#include <gtest/gtest.h>

#include <vector>

#include "SeqNumRanges.hpp"

namespace {

using fix_common::SeqNumRange;
namespace ranges = fix_common::seq_num_ranges;

std::vector<SeqNumRange> made_of(std::initializer_list<SeqNumRange> entries) {
    return std::vector<SeqNumRange>{entries};
}

TEST(SeqNumRangesTest, AppendStartsARangeAndExtendsIt) {
    std::vector<SeqNumRange> recorded;
    ranges::append(recorded, 10);
    ranges::append(recorded, 11);
    ranges::append(recorded, 12);

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 10);
    EXPECT_EQ(recorded[0].to_seq_num, 12);
}

TEST(SeqNumRangesTest, AppendStartsANewRangeWhenANumberIsSkipped) {
    // The skipped number held something the venue cannot replay -- a heartbeat, most often --
    // and the gap between the two runs is the whole point of keeping this.
    std::vector<SeqNumRange> recorded;
    ranges::append(recorded, 10);
    ranges::append(recorded, 12);

    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[1].from_seq_num, 12);
    EXPECT_FALSE(ranges::covers(recorded, 11));
}

TEST(SeqNumRangesTest, AppendIgnoresANumberAlreadyCovered) {
    // A replay re-sends reports onto numbers this already covers. Recording them again would
    // describe the same numbers twice and, worse, out of order.
    std::vector<SeqNumRange> recorded = made_of({{10, 20}});
    ranges::append(recorded, 15);

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].to_seq_num, 20);
}

TEST(SeqNumRangesTest, MergeJoinsRangesThatAbut) {
    // 5..9 and 10..12 are one run of numbers, and leaving them as two would still answer every
    // query correctly -- but the records travel on every session update, so they are joined.
    std::vector<SeqNumRange> recorded = made_of({{5, 9}});
    ranges::merge(recorded, made_of({{10, 12}}));

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 5);
    EXPECT_EQ(recorded[0].to_seq_num, 12);
}

TEST(SeqNumRangesTest, MergeIsIdempotentAndAcceptsOverlap) {
    // The sequencer merges what a gateway reports without knowing what it has already been
    // told. An update repeated after a retry, or one overlapping the last, must not corrupt it.
    std::vector<SeqNumRange> recorded = made_of({{5, 20}});
    ranges::merge(recorded, made_of({{5, 20}}));
    ranges::merge(recorded, made_of({{15, 25}}));

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 5);
    EXPECT_EQ(recorded[0].to_seq_num, 25);
}

TEST(SeqNumRangesTest, MergeSortsRangesArrivingOutOfOrder) {
    std::vector<SeqNumRange> recorded = made_of({{30, 40}});
    ranges::merge(recorded, made_of({{5, 10}}));

    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded[0].from_seq_num, 5);
    EXPECT_EQ(recorded[1].from_seq_num, 30);
}

TEST(SeqNumRangesTest, CoversIsInclusiveAtBothEnds) {
    const std::vector<SeqNumRange> recorded = made_of({{10, 12}});

    EXPECT_FALSE(ranges::covers(recorded, 9));
    EXPECT_TRUE(ranges::covers(recorded, 10));
    EXPECT_TRUE(ranges::covers(recorded, 12));
    EXPECT_FALSE(ranges::covers(recorded, 13));
}

TEST(SeqNumRangesTest, CountInCountsOnlyTheOverlap) {
    // This is what a resend asks the sequencer for, so counting one too many means one report
    // too many comes back and lands on a number that held something else.
    const std::vector<SeqNumRange> recorded = made_of({{10, 19}, {30, 39}});

    EXPECT_EQ(ranges::count_in(recorded, 1, 100), 20);
    EXPECT_EQ(ranges::count_in(recorded, 15, 34), 10); // 15..19 and 30..34
    EXPECT_EQ(ranges::count_in(recorded, 20, 29), 0);  // entirely between the two runs
    EXPECT_EQ(ranges::count_in(recorded, 19, 19), 1);  // a single number at a boundary
}

TEST(SeqNumRangesTest, CountInIsEmptyForAnInvertedRange) {
    const std::vector<SeqNumRange> recorded = made_of({{10, 19}});
    EXPECT_EQ(ranges::count_in(recorded, 19, 10), 0);
}

TEST(SeqNumRangesTest, FromSeqNumClipsTheRangeItStartsIn) {
    // Used to report only what has happened since the last update, so a range straddling the
    // watermark must come back clipped rather than whole or dropped.
    const std::vector<SeqNumRange> recorded = made_of({{10, 19}, {30, 39}});
    const std::vector<SeqNumRange> tail = ranges::from_seq_num(recorded, 15);

    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0].from_seq_num, 15);
    EXPECT_EQ(tail[0].to_seq_num, 19);
    EXPECT_EQ(tail[1].from_seq_num, 30);
}

TEST(SeqNumRangesTest, TrimKeepsTheMostRecentNumbersAndDropsTheRest) {
    // What is dropped becomes uncovered, which is the same state as a number that never held a
    // report: the venue gap-fills it. Retention therefore shortens a resend's reach without
    // ever making one wrong, which is why there is no second mechanism to keep in step.
    std::vector<SeqNumRange> recorded = made_of({{1, 100}});
    ranges::trim(recorded, 10);

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 91);
    EXPECT_EQ(recorded[0].to_seq_num, 100);
    EXPECT_FALSE(ranges::covers(recorded, 90));
}

TEST(SeqNumRangesTest, TrimLeavesAShortRecordAlone) {
    std::vector<SeqNumRange> recorded = made_of({{1, 5}});
    ranges::trim(recorded, 100);

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 1);
}

TEST(SeqNumRangesTest, TrimDropsWholeRangesThatFallBelowTheWindow) {
    std::vector<SeqNumRange> recorded = made_of({{1, 10}, {90, 100}});
    ranges::trim(recorded, 11);

    ASSERT_EQ(recorded.size(), 1u);
    EXPECT_EQ(recorded[0].from_seq_num, 90);
}

TEST(SeqNumRangesTest, AnEmptyRecordCoversNothingAndCountsNothing) {
    // The state a gateway is in when it takes on a session whose previous instance died
    // without reporting. Every number is uncovered, so the whole range is gap-filled and
    // nothing is guessed at.
    const std::vector<SeqNumRange> recorded;

    EXPECT_FALSE(ranges::covers(recorded, 1));
    EXPECT_EQ(ranges::count_in(recorded, 1, 1000), 0);
    EXPECT_TRUE(ranges::from_seq_num(recorded, 1).empty());
}

} // namespaces
