// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "EngineReportPacing.hpp"

namespace matching_engine::tests {

namespace {

// What EngineReportPacing asks the thread to do, in order.
struct RecordingPacingLinks : public EngineReportPacingLinksInterface {
    void pause_reading_commands() override {
        actions.emplace_back("pause");
    }
    void resume_reading_commands() override {
        actions.emplace_back("resume");
    }
    void request_written_notification(const pubsub_itc_fw::ConnectionID& connection) override {
        actions.push_back("notify " + std::to_string(connection.get_value()));
    }
    std::vector<std::string> actions;
};

class EngineReportPacingTest : public ::testing::Test {
  protected:
    // Sends @p count reports on @p connection, answering commands from @p first_seq_no on.
    void send(const pubsub_itc_fw::ConnectionID& connection, int64_t count, int64_t first_seq_no = 1) {
        for (int64_t index = 0; index < count; ++index) {
            pacing_.report_sent(connection, first_seq_no + index);
        }
    }

    // Every line logged so far. The test logger hands lines over in the background, so it is flushed first.
    [[nodiscard]] const std::vector<std::string>& lines() {
        logger_.flush();
        return log_;
    }

    [[nodiscard]] bool logged(const std::string& text) {
        logger_.flush();
        for (const std::string& line : log_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static constexpr int64_t limit = EngineReportPacing::pause_at_reports;

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingPacingLinks links_;
    EngineReportPacing pacing_{links_, logger_};
    const pubsub_itc_fw::ConnectionID leader_{11};
    const pubsub_itc_fw::ConnectionID follower_{12};
};

} // namespaces

TEST_F(EngineReportPacingTest, NothingIsPausedOrAskedBelowTheLimit) {
    pacing_.pace_on(leader_);
    send(leader_, limit - 1);
    EXPECT_TRUE(links_.actions.empty());
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(pacing_.not_known_written(leader_), limit - 1);
}

TEST_F(EngineReportPacingTest, AtTheLimitOnTheLeadersConnectionTheCommandsArePausedAndNotificationIsAskedFor) {
    pacing_.pace_on(leader_);
    send(leader_, limit);
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 11", "pause"}));
    EXPECT_TRUE(pacing_.paused());
}

TEST_F(EngineReportPacingTest, TheNotificationResumesTheCommandsAndClearsWhatWasSentBeforeTheRequest) {
    pacing_.pace_on(leader_);
    send(leader_, limit);
    pacing_.connection_written(leader_);
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 11", "pause", "resume"}));
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(pacing_.not_known_written(leader_), 0);
}

TEST_F(EngineReportPacingTest, ReportsSentAfterTheRequestAreNotTakenAsWrittenByIt) {
    // Commands the reactor had already read when the pause took effect are applied, and their reports sent, after the
    // request; the notification says nothing about them.
    pacing_.pace_on(leader_);
    send(leader_, limit);
    send(leader_, 10, limit + 1);
    pacing_.connection_written(leader_);
    EXPECT_EQ(pacing_.not_known_written(leader_), 10);
    EXPECT_FALSE(pacing_.paused());
}

TEST_F(EngineReportPacingTest, IfAsManyAgainWereSentWhilePausedItStaysPausedAndAsksAgain) {
    pacing_.pace_on(leader_);
    send(leader_, limit);
    send(leader_, limit, limit + 1);
    pacing_.connection_written(leader_);
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 11", "pause", "notify 11"}));
    EXPECT_TRUE(pacing_.paused());
    pacing_.connection_written(leader_);
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(pacing_.not_known_written(leader_), 0);
}

TEST_F(EngineReportPacingTest, TheFollowersConnectionIsCountedButNeverPausesAnything) {
    pacing_.pace_on(leader_);
    send(follower_, limit * 3);
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 12"}));
    EXPECT_FALSE(pacing_.paused());
    pacing_.connection_written(follower_);
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 12", "notify 12"})) << "still a limit's worth not known written, so it asks again";
    EXPECT_FALSE(pacing_.paused());
}

TEST_F(EngineReportPacingTest, WithNoConnectionPacedNothingIsPaused) {
    send(leader_, limit * 2);
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 11"}));
}

TEST_F(EngineReportPacingTest, ANotificationNotAskedForChangesNothing) {
    pacing_.pace_on(leader_);
    send(leader_, 5);
    pacing_.connection_written(leader_);
    EXPECT_EQ(pacing_.not_known_written(leader_), 5);
    EXPECT_TRUE(links_.actions.empty());
}

TEST_F(EngineReportPacingTest, LosingThePacedConnectionWhilePausedResumesTheCommands) {
    // No notification comes on a connection that is gone; waiting for one would stop the engine for good.
    pacing_.pace_on(leader_);
    send(leader_, limit);
    pacing_.connection_lost(leader_, "primary");
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(links_.actions.back(), "resume");
}

TEST_F(EngineReportPacingTest, LosingAConnectionWithReportsNotKnownWrittenLogsOneErrorNamingThem) {
    pacing_.pace_on(leader_);
    send(leader_, 7, 100);
    pacing_.connection_lost(leader_, "primary");
    ASSERT_EQ(lines().size(), 1U);
    EXPECT_TRUE(logged("ERROR")) << log_.front();
    EXPECT_TRUE(logged("primary sequencer is lost with 7 report(s)")) << log_.front();
    EXPECT_TRUE(logged("answering commands 100 to 106")) << log_.front();
}

TEST_F(EngineReportPacingTest, LosingAConnectionWithEveryReportWrittenLogsNothing) {
    pacing_.pace_on(leader_);
    send(leader_, limit);
    pacing_.connection_written(leader_);
    pacing_.connection_lost(leader_, "primary");
    EXPECT_TRUE(lines().empty());
}

TEST_F(EngineReportPacingTest, PacingOnANewConnectionResumesAPauseOnTheOld) {
    pacing_.pace_on(leader_);
    send(leader_, limit);
    ASSERT_TRUE(pacing_.paused());
    pacing_.pace_on(follower_);
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(links_.actions.back(), "resume");
}

TEST_F(EngineReportPacingTest, WithPausingTurnedOffNothingIsPausedButTheReportsAreStillCounted) {
    pacing_.set_pausing(false);
    pacing_.pace_on(leader_);
    send(leader_, limit * 2);
    EXPECT_FALSE(pacing_.paused());
    EXPECT_EQ(links_.actions, (std::vector<std::string>{"notify 11"}));
    const EngineReportPacing::Summary summary = pacing_.take_summary();
    EXPECT_EQ(summary.most_not_known_written, limit * 2);
    EXPECT_EQ(summary.pauses, 0);
}

TEST_F(EngineReportPacingTest, TheSummarySaysTheMostWaitingAndThePausesAndStartsAgain) {
    pacing_.pace_on(leader_);
    send(leader_, limit + 5);
    pacing_.connection_written(leader_);
    send(leader_, 2, limit + 6);
    EngineReportPacing::Summary summary = pacing_.take_summary();
    EXPECT_EQ(summary.most_not_known_written, limit + 5);
    EXPECT_EQ(summary.pauses, 1);
    summary = pacing_.take_summary();
    EXPECT_EQ(summary.most_not_known_written, 0);
    EXPECT_EQ(summary.pauses, 0);
}

TEST_F(EngineReportPacingTest, PacingOnAConnectionAlreadyAtTheLimitPausesAtOnce) {
    send(follower_, limit + 3);
    pacing_.pace_on(follower_);
    EXPECT_TRUE(pacing_.paused()) << "the follower became the leader with a limit's worth outstanding";
}

} // namespaces
