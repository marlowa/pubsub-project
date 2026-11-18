// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "CancelInPieces.hpp"

namespace matching_engine::tests {

namespace {

// A held message is its name. Acting on a message whose name is "starts a cancel" starts another cancel of the given
// number of pieces, as applying such a command would.
struct FakeLinks : public CancelInPiecesLinksInterface<std::string> {
    void set_commands_paused(bool paused) override {
        actions.emplace_back(paused ? "pause" : "resume");
    }
    void post_continuation() override {
        ++continuations_posted;
    }
    bool carry_out_piece() override {
        actions.emplace_back("piece");
        --pieces_left;
        return pieces_left == 0;
    }
    [[nodiscard]] bool cancel_under_way() const override {
        return pieces_left > 0;
    }
    void act_on(std::string&& message) override {
        actions.push_back("act on " + message);
        if (message == "starts a cancel") {
            pieces_left = 2;
            cancel_in_pieces->after_command();
        }
    }

    int pieces_left{0};
    int continuations_posted{0};
    std::vector<std::string> actions;
    CancelInPieces<std::string>* cancel_in_pieces{nullptr};
};

class CancelInPiecesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        links_.cancel_in_pieces = &pieces_;
    }

    // A command was applied that left a cancel of @p pieces pieces, the first carried out with the command.
    void start_cancel(int pieces) {
        links_.pieces_left = pieces;
        pieces_.after_command();
    }

    // Delivers every continuation posted, as the run loop would, until none is on its way.
    void run_continuations() {
        while (links_.continuations_posted > 0) {
            --links_.continuations_posted;
            pieces_.continuation_arrived();
        }
    }

    FakeLinks links_;
    CancelInPieces<std::string> pieces_{links_};
};

} // un-named namespace

// A command that leaves no pieces pauses nothing and asks for nothing.
TEST_F(CancelInPiecesTest, ACommandThatFinishesAtOnceChangesNothing) {
    pieces_.after_command();
    EXPECT_TRUE(links_.actions.empty());
    EXPECT_EQ(links_.continuations_posted, 0);
    EXPECT_FALSE(pieces_.holds());
}

// The commands are paused while a cancel has pieces left, each piece is asked for by one message to the thread, and the
// commands are read again after the last.
TEST_F(CancelInPiecesTest, TheCommandsArePausedUntilTheLastPieceAndEachPieceIsAskedForOnce) {
    start_cancel(3);
    EXPECT_TRUE(pieces_.holds());
    EXPECT_EQ(links_.continuations_posted, 1);
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause", "piece", "piece", "piece", "resume"}));
    EXPECT_FALSE(pieces_.holds());
}

// What arrived meanwhile is acted on in the order it arrived, after the last piece and before the commands are read
// again.
TEST_F(CancelInPiecesTest, MessagesHeldDuringTheCancelAreActedOnAfterItInOrder) {
    start_cancel(2);
    pieces_.hold("first");
    pieces_.hold("second");
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause", "piece", "piece", "act on first", "act on second", "resume"}));
    EXPECT_EQ(pieces_.held(), 0U);
}

// A held command that starts another cancel in pieces stops the rest being acted on until that one is finished too, and
// the commands stay paused throughout.
TEST_F(CancelInPiecesTest, AHeldCommandThatStartsAnotherCancelWaitsForItBeforeTheRest) {
    start_cancel(1 + 1);
    pieces_.hold("starts a cancel");
    pieces_.hold("after both");
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause", "piece", "piece", "act on starts a cancel", "piece", "piece", "act on after both", "resume"}));
}

// While report pacing holds the commands, no piece is carried out; when it lets them go, the next piece is asked for, and
// the commands stay paused until the cancel is finished.
TEST_F(CancelInPiecesTest, NoPieceIsCarriedOutWhilePacingHoldsTheCommands) {
    start_cancel(3);
    pieces_.pacing_pauses();
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause"})) << "the continuation on its way finds pacing holding the commands";
    EXPECT_EQ(links_.continuations_posted, 0) << "and asks for none until pacing lets go";
    pieces_.pacing_resumes();
    EXPECT_EQ(links_.continuations_posted, 1);
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause", "piece", "piece", "piece", "resume"}));
}

// Pacing letting the commands go during a cancel does not read them: the cancel still holds them.
TEST_F(CancelInPiecesTest, PacingLettingGoDuringACancelDoesNotReadTheCommands) {
    pieces_.pacing_pauses();
    start_cancel(2);
    pieces_.pacing_resumes();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause"}));
    run_continuations();
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"pause", "piece", "piece", "resume"}));
}

} // namespaces
