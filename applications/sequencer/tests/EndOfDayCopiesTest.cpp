// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include "EndOfDayCopies.hpp"

namespace sequencer::tests {

namespace {

using Verdict = EndOfDayCopies::Verdict;

constexpr int64_t limit_ns = 10'000;
constexpr int64_t begin_seq_no = 500;
const pubsub_itc_fw::ConnectionID first_publisher{11};
const pubsub_itc_fw::ConnectionID second_publisher{12};

} // namespaces

TEST(EndOfDayCopiesTest, WithNoFollowerSequencerAndNoPublisherEveryCopyIsComplete) {
    EndOfDayCopies copies(0, limit_ns);
    copies.start(begin_seq_no, 0);
    const EndOfDayCopies::Assessment assessment = copies.assess(std::nullopt, 0);
    EXPECT_EQ(assessment.verdict, Verdict::EveryCopyComplete);
    EXPECT_NE(assessment.explanation.find("there is no follower sequencer"), std::string::npos) << assessment.explanation;
}

TEST(EndOfDayCopiesTest, TheFollowerSequencerMustHoldTheBeginItself) {
    EndOfDayCopies copies(0, limit_ns);
    copies.start(begin_seq_no, 0);
    EXPECT_EQ(copies.assess(begin_seq_no - 1, 0).verdict, Verdict::Waiting);
    EXPECT_EQ(copies.assess(begin_seq_no, 0).verdict, Verdict::EveryCopyComplete);
}

TEST(EndOfDayCopiesTest, EveryPublisherWaitedForMustHoldTheBegin) {
    EndOfDayCopies copies(2, limit_ns);
    copies.start(begin_seq_no, 0);
    copies.publisher_holds(first_publisher, begin_seq_no);
    EXPECT_EQ(copies.assess(std::nullopt, 0).verdict, Verdict::Waiting) << "one of two";
    copies.publisher_holds(second_publisher, begin_seq_no + 3);
    EXPECT_EQ(copies.assess(std::nullopt, 0).verdict, Verdict::EveryCopyComplete);
}

TEST(EndOfDayCopiesTest, OnePublisherAnsweringTwiceCountsOnce) {
    EndOfDayCopies copies(2, limit_ns);
    copies.start(begin_seq_no, 0);
    copies.publisher_holds(first_publisher, begin_seq_no - 1);
    copies.publisher_holds(first_publisher, begin_seq_no);
    EXPECT_EQ(copies.assess(std::nullopt, 0).verdict, Verdict::Waiting);
}

TEST(EndOfDayCopiesTest, TheLimitIsCountedFromTheFirstStartForTheSameBegin) {
    EndOfDayCopies copies(1, limit_ns);
    copies.start(begin_seq_no, 100);
    copies.start(begin_seq_no, 5000);
    EXPECT_EQ(copies.assess(std::nullopt, 100 + limit_ns - 1).verdict, Verdict::Waiting);
    const EndOfDayCopies::Assessment passed = copies.assess(std::nullopt, 100 + limit_ns);
    EXPECT_EQ(passed.verdict, Verdict::LimitPassed);
    EXPECT_NE(passed.explanation.find("0 of the 1 publisher(s)"), std::string::npos) << passed.explanation;
}

TEST(EndOfDayCopiesTest, ADifferentBeginStartsTheWaitAfresh) {
    EndOfDayCopies copies(1, limit_ns);
    copies.start(begin_seq_no, 0);
    copies.start(begin_seq_no + 10, 8000);
    EXPECT_EQ(copies.begin_seq_no(), begin_seq_no + 10);
    EXPECT_EQ(copies.assess(std::nullopt, limit_ns).verdict, Verdict::Waiting);
}

TEST(EndOfDayCopiesTest, ForgettingClearsTheWaitAndTheAnswers) {
    EndOfDayCopies copies(1, limit_ns);
    copies.start(begin_seq_no, 0);
    copies.publisher_holds(first_publisher, begin_seq_no);
    copies.forget();
    EXPECT_FALSE(copies.started());
    copies.start(begin_seq_no, 0);
    EXPECT_EQ(copies.assess(std::nullopt, 0).verdict, Verdict::Waiting) << "the publisher is asked again";
}

} // namespaces
