// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "FollowerReportComparison.hpp"
#include "HeldEngineReport.hpp"

namespace matching_engine::tests {

namespace {

using Reason = pubsub_itc_fw_app::FollowerCannotLeadReason;

// What FollowerReportComparison asks the thread to do.
struct RecordingLinks : public FollowerReportComparisonLinksInterface {
    void stop_leading_for_good(int64_t command, Reason reason) override {
        stopped.push_back({command, reason});
    }
    void difference_found() override {
        ++differences;
    }
    void kept_reports_sent(int64_t sent, int64_t reported_through) override {
        sent_events.push_back({sent, reported_through});
    }
    std::vector<std::pair<int64_t, Reason>> stopped;
    int64_t differences{0};
    std::vector<std::pair<int64_t, int64_t>> sent_events;
};

class FollowerReportComparisonTest : public ::testing::Test {
  protected:
    void SetUp() override {
        comparison_ = std::make_unique<FollowerReportComparison>(links_, logger_);
    }

    // Keeps a report of command @p command whose bytes are @p text.
    void keep(int64_t command, const std::string& text) {
        HeldEngineReport report;
        report.seq_no = command;
        report.payload.assign(text.begin(), text.end());
        report.poss_resend = true;
        comparison_->keep(std::move(report));
    }

    // The leader's report record @p record, answering @p command, whose bytes are @p text.
    void leader(int64_t record, int64_t command, const std::string& text, int64_t reports_in_wal_through = 0) {
        comparison_->leader_report(record, command, reinterpret_cast<const uint8_t*>(text.data()), text.size(), reports_in_wal_through);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<FollowerReportComparison> comparison_;
};

} // namespaces

TEST_F(FollowerReportComparisonTest, ReportsThatMatchTheLeadersAreNoDifference) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted-1");
    keep(2, "accepted-2");
    leader(10, 1, "accepted-1");
    leader(11, 2, "accepted-2");
    leader(12, 3, "x") /* a later command: the reports of 1 and 2, compared, are dropped */;
    EXPECT_EQ(comparison_->differences(), 1) << "command 3 was not kept: the leader sent a report this instance did not produce";
    EXPECT_EQ(comparison_->kept(), 0U);
}

TEST_F(FollowerReportComparisonTest, AReportThatDiffersFromTheLeadersRefusesTheLead) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    leader(10, 1, "rejected");
    EXPECT_EQ(comparison_->differences(), 1);
    EXPECT_EQ(links_.differences, 1);
    EXPECT_TRUE(comparison_->refused_to_lead());
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].first, 1);
    EXPECT_EQ(links_.stopped[0].second, Reason::ReportDiffers);
}

TEST_F(FollowerReportComparisonTest, AKeptReportTheLeaderNeverSentIsADifference) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    leader(10, 2, "accepted-2");
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].first, 1);
    EXPECT_EQ(links_.stopped[0].second, Reason::ReportNotSent);
}

TEST_F(FollowerReportComparisonTest, ALeadersReportOfACommandNotKeptIsADifferenceOnlyWhereReportsAreKept) {
    comparison_->start_keeping_after(5);
    leader(10, 3, "before keeping began");
    leader(11, 7, "the WAL held every report of it when the catch-up started", 7);
    EXPECT_EQ(comparison_->differences(), 0);
    leader(12, 8, "not produced here", 7);
    EXPECT_EQ(comparison_->differences(), 1);
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].second, Reason::ReportNotProduced);
}

TEST_F(FollowerReportComparisonTest, AReportRecordAccountedForAlreadyIsPassedOver) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    leader(10, 1, "accepted");
    leader(10, 1, "different, but the same record again");
    leader(9, 1, "different, an earlier record");
    EXPECT_EQ(comparison_->differences(), 0);
}

TEST_F(FollowerReportComparisonTest, AReportWrittenAgainByANewLeaderIsRecognised) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    keep(1, "filled");
    leader(10, 1, "accepted");
    leader(11, 1, "accepted") /* the same report, written again into the WAL */;
    EXPECT_EQ(comparison_->differences(), 0);
    leader(12, 1, "filled");
    EXPECT_EQ(comparison_->differences(), 0);
    keep(2, "accepted-2");
    leader(13, 2, "accepted-2");
    leader(14, 1, "a report of an earlier command") /* a sequencer that took the lead writes its kept reports again */;
    EXPECT_EQ(comparison_->differences(), 0);
}

TEST_F(FollowerReportComparisonTest, KeepingStartsAfterTheCatchUpsPositionOnlyWhenNothingIsKept) {
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    comparison_->start_keeping_after(10) /* a request made again: reports are already kept, so the start stays */;
    leader(20, 5, "not produced here");
    EXPECT_EQ(comparison_->differences(), 2) << "the kept report of 1 was not sent, and 5 was not produced";
}

TEST_F(FollowerReportComparisonTest, LaterDifferencesAreCountedButTheRefusalHappensOnce) {
    comparison_->start_keeping_after(0);
    keep(1, "a");
    keep(2, "b");
    leader(10, 1, "x");
    leader(11, 2, "y");
    EXPECT_EQ(comparison_->differences(), 2);
    EXPECT_EQ(links_.differences, 2);
    EXPECT_EQ(links_.stopped.size(), 1U);
}

TEST_F(FollowerReportComparisonTest, AVoidLearnedTooLateRefusesTheLeadOnce) {
    comparison_->refuse_to_lead(42, Reason::AppliedVoidedCommand, "it applied a command before it was told that a later command voided it");
    comparison_->refuse_to_lead(43, Reason::AppliedVoidedCommand, "again");
    EXPECT_TRUE(comparison_->refused_to_lead());
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].first, 42);
    EXPECT_EQ(comparison_->differences(), 0) << "a void is not a difference between reports";
}

TEST_F(FollowerReportComparisonTest, AFullStoreRefusesTheLeadAndAMissingReportIsThenNoDifference) {
    comparison_->start_keeping_after(0);
    for (size_t command = 1; command <= FollowerReportComparison::max_kept_reports; ++command) {
        keep(static_cast<int64_t>(command), "r");
    }
    EXPECT_FALSE(comparison_->has_room());
    keep(static_cast<int64_t>(FollowerReportComparison::max_kept_reports) + 1, "not kept");
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].second, Reason::KeptReportsFull);
    EXPECT_EQ(comparison_->kept(), FollowerReportComparison::max_kept_reports);
    // Every kept report compared and matched, then a command whose report was not kept for want of room.
    for (size_t command = 1; command <= FollowerReportComparison::max_kept_reports; ++command) {
        leader(static_cast<int64_t>(command), static_cast<int64_t>(command), "r");
    }
    leader(static_cast<int64_t>(FollowerReportComparison::max_kept_reports) + 1, static_cast<int64_t>(FollowerReportComparison::max_kept_reports) + 1,
           "not kept");
    EXPECT_EQ(comparison_->differences(), 0);
}

TEST_F(FollowerReportComparisonTest, TakingTheLeadHandsOnTheReportsTheWalLacksAndDropsTheRest) {
    comparison_->start_keeping_after(0);
    keep(1, "a");
    keep(2, "b");
    keep(3, "c");
    std::vector<std::string> taken;
    const int64_t sent = comparison_->take_reports_after(1, [&taken](HeldEngineReport&& report) {
        taken.emplace_back(report.payload.begin(), report.payload.end());
        EXPECT_TRUE(report.poss_resend);
    });
    EXPECT_EQ(sent, 2);
    EXPECT_EQ(taken, (std::vector<std::string>{"b", "c"}));
    EXPECT_EQ(comparison_->kept(), 0U);
    ASSERT_EQ(links_.sent_events.size(), 1U);
    EXPECT_EQ(links_.sent_events[0], (std::pair<int64_t, int64_t>{2, 1}));
}

} // namespaces
