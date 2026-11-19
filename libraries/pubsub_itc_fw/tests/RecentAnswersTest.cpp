// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/RecentAnswers.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw::tests {

TEST(RecentAnswersTest, TheFirstAnswerToACommandIsFoundWhereItWasNoted) {
    RecentAnswers answers(8, 1);
    answers.note(3, 5, WalPosition{0, 500});
    answers.note(3, 9, WalPosition{0, 900});
    const RecentAnswers::Lookup found = answers.lookup(3);
    EXPECT_EQ(found.kind, RecentAnswers::Kind::found);
    EXPECT_EQ(found.answer_seq_no, 5) << "the first answer, not a later one";
    EXPECT_EQ(found.position.offset, 500U);
}

TEST(RecentAnswersTest, ARecentCommandWithNoAnswerNotedHasNoneYet) {
    RecentAnswers answers(8, 1);
    answers.note(3, 5, WalPosition{0, 500});
    EXPECT_EQ(answers.lookup(4).kind, RecentAnswers::Kind::none_yet);
    EXPECT_EQ(answers.lookup(11).kind, RecentAnswers::Kind::none_yet) << "place 3 holds an earlier command, 3, so 11 has no answer yet";
}

// A command whose place a later command's answer has taken may have had an answer, displaced; only a search can tell.
TEST(RecentAnswersTest, ACommandWhosePlaceALaterCommandTookIsNotKnown) {
    RecentAnswers answers(8, 1);
    answers.note(3, 5, WalPosition{0, 500});
    answers.note(11, 13, WalPosition{0, 1300});
    EXPECT_EQ(answers.lookup(3).kind, RecentAnswers::Kind::not_known);
    EXPECT_EQ(answers.lookup(11).kind, RecentAnswers::Kind::found);
}

TEST(RecentAnswersTest, ACommandFromBeforeTheTableStartedNotingIsNotKnown) {
    RecentAnswers answers(8, 100);
    EXPECT_EQ(answers.lookup(99).kind, RecentAnswers::Kind::not_known);
    EXPECT_EQ(answers.lookup(100).kind, RecentAnswers::Kind::none_yet);
}

TEST(RecentAnswersTest, CuttingTheWalBackForgetsEveryAnswerAndNotesAgainFromThere) {
    RecentAnswers answers(8, 1);
    answers.note(3, 5, WalPosition{0, 500});
    answers.forget_from(4);
    EXPECT_EQ(answers.lookup(3).kind, RecentAnswers::Kind::not_known) << "its answer may have been among the records discarded";
    EXPECT_EQ(answers.lookup(4).kind, RecentAnswers::Kind::none_yet);
    answers.note(4, 6, WalPosition{0, 600});
    EXPECT_EQ(answers.lookup(4).kind, RecentAnswers::Kind::found);
}

TEST(RecentAnswersTest, ANumberOfPlacesThatIsNotAPowerOfTwoIsRefused) {
    EXPECT_THROW(RecentAnswers(6, 1), PreconditionAssertion);
    EXPECT_THROW(RecentAnswers(1, 1), PreconditionAssertion);
}

} // namespaces
