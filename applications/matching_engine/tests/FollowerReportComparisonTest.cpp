// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

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

// Lowers this process's limit on locked memory to @p bytes for as long as it exists, and restores it after.
class LockedMemoryLimitLowered {
  public:
    ~LockedMemoryLimitLowered() {
        ::setrlimit(RLIMIT_MEMLOCK, &saved_);
    }
    explicit LockedMemoryLimitLowered(rlim_t bytes) {
        ::getrlimit(RLIMIT_MEMLOCK, &saved_);
        rlimit lowered = saved_;
        lowered.rlim_cur = bytes;
        ::setrlimit(RLIMIT_MEMLOCK, &lowered);
    }
    LockedMemoryLimitLowered(const LockedMemoryLimitLowered&) = delete;
    LockedMemoryLimitLowered& operator=(const LockedMemoryLimitLowered&) = delete;
    LockedMemoryLimitLowered(LockedMemoryLimitLowered&&) = delete;
    LockedMemoryLimitLowered& operator=(LockedMemoryLimitLowered&&) = delete;

  private:
    rlimit saved_{};
};

class FollowerReportComparisonTest : public ::testing::Test {
  protected:
    void SetUp() override {
        comparison_ = std::make_unique<FollowerReportComparison>(links_, logger_, FollowerReportComparison::max_kept_reports, 1024 * 1024);
    }

    // Replaces the store with one holding at most @p most_reports reports and @p most_bytes bytes of them.
    void use_a_store_of(size_t most_reports, size_t most_bytes) {
        comparison_ = std::make_unique<FollowerReportComparison>(links_, logger_, most_reports, most_bytes);
    }

    // Keeps a report of command @p command, produced by applying it, whose bytes are @p text.
    void keep(int64_t command, const std::string& text) {
        keep_report_of(command, command, text);
    }

    // The bytes of the reports of commands after @p reported_through, handed on as if this instance took the lead.
    std::vector<std::string> take_after(int64_t reported_through) {
        std::vector<std::string> taken;
        static_cast<void>(comparison_->take_reports_after(
            reported_through, [&taken](HeldEngineReport&& report) { taken.emplace_back(report.payload.begin(), report.payload.end()); }));
        return taken;
    }

    // The leader's report record @p record, produced by command @p command, whose bytes are @p text.
    void leader(int64_t record, int64_t command, const std::string& text, int64_t reports_in_wal_through = 0) {
        comparison_->leader_report(record, command, reinterpret_cast<const uint8_t*>(text.data()), text.size(), reports_in_wal_through);
    }

    // A report this instance produced while applying command @p produced_by, answering command @p answers, which is zero
    // for a report that answers no member's command, such as a cancel of a session's orders. It is kept as the engine
    // keeps it.
    void keep_report_of(int64_t produced_by, int64_t answers, const std::string& text) {
        comparison_->keep(produced_by, answers, fix_common::SessionIdentity{}, text.size(), [&text](uint8_t* destination, size_t room) -> size_t {
            EXPECT_GE(room, text.size());
            std::memcpy(destination, text.data(), text.size());
            return text.size();
        });
    }

    // The leader's report record @p record, produced while applying command @p produced_by and answering @p answers, as
    // the engine passes it on for comparison.
    void leader_report_of(int64_t record, int64_t produced_by, int64_t answers, const std::string& text) {
        static_cast<void>(answers); // the leader's report is compared under the command that produced it
        leader(record, produced_by, text);
    }

    // The command numbers the reports of commands after @p reported_through are handed on with, as if this instance took
    // the lead: the number each answers, which is what the sequencer routes it by.
    std::vector<std::pair<std::string, int64_t>> take_after_with_numbers(int64_t reported_through) {
        std::vector<std::pair<std::string, int64_t>> taken;
        static_cast<void>(comparison_->take_reports_after(reported_through, [&taken](HeldEngineReport&& report) {
            taken.emplace_back(std::string(report.payload.begin(), report.payload.end()), report.seq_no);
        }));
        return taken;
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

// The fault of BUG-0263. Command 11 cancels the orders of a session that has not come back, and each cancel answers no
// member's command, so it carries the number 0. Both engines produce the same reports, so there is no difference.
TEST_F(FollowerReportComparisonTest, AReportAnsweringNoCommandIsComparedUnderTheCommandThatProducedIt) {
    comparison_->start_keeping_after(0);
    keep_report_of(10, 10, "accepted-10");
    keep_report_of(11, 0, "cancelled-by-11");
    keep_report_of(12, 12, "accepted-12");
    leader_report_of(100, 10, 10, "accepted-10");
    leader_report_of(101, 11, 0, "cancelled-by-11");
    leader_report_of(102, 12, 12, "accepted-12");
    leader_report_of(103, 13, 13, "accepted-13") /* a later command: everything before it has been compared */;
    EXPECT_EQ(comparison_->differences(), 1) << "only command 13, which was not kept here, is a difference";
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].first, 13);
    EXPECT_EQ(links_.stopped[0].second, Reason::ReportNotProduced);
}

// Taking the lead hands on every kept report of a command after the last the WAL holds every report of, including one
// that answers no member's command, and hands it on with the number it answers.
TEST_F(FollowerReportComparisonTest, TakingTheLeadHandsOnAReportAnsweringNoCommandThatTheWalLacks) {
    comparison_->start_keeping_after(0);
    keep_report_of(10, 10, "accepted-10");
    keep_report_of(11, 0, "cancelled-by-11");
    keep_report_of(12, 12, "accepted-12");
    const std::vector<std::pair<std::string, int64_t>> taken = take_after_with_numbers(10);
    ASSERT_EQ(taken.size(), 2U);
    EXPECT_EQ(taken[0], (std::pair<std::string, int64_t>{"cancelled-by-11", 0}));
    EXPECT_EQ(taken[1], (std::pair<std::string, int64_t>{"accepted-12", 12}));
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
    EXPECT_FALSE(comparison_->refused_to_lead()) << "the store is full, but nothing has yet been refused room";
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

TEST_F(FollowerReportComparisonTest, BytesFreedAtTheStartOfTheStoreAreUsedAgain) {
    use_a_store_of(10, 16);
    comparison_->start_keeping_after(0);
    keep(1, "aaaaaa");
    keep(2, "bbbbbb");
    leader(10, 1, "aaaaaa");
    leader(11, 2, "bbbbbb") /* the report of 1 leaves, freeing the first six bytes */;
    keep(3, "cccccc") /* four bytes are left at the end, too few, so it goes at the start */;
    leader(12, 3, "cccccc");
    keep(4, "dddddddddd") /* the report of 2 has left: the ten bytes after the report of 3 are free */;
    leader(13, 4, "dddddddddd");
    EXPECT_EQ(comparison_->differences(), 0);
    EXPECT_FALSE(comparison_->refused_to_lead());
    EXPECT_EQ(comparison_->kept(), 1U);
    EXPECT_EQ(comparison_->kept_bytes(), 10U);
    EXPECT_EQ(take_after(3), (std::vector<std::string>{"dddddddddd"}));
}

TEST_F(FollowerReportComparisonTest, TheBytesOfAReportStillKeptAreNeverOverwritten) {
    use_a_store_of(10, 16);
    comparison_->start_keeping_after(0);
    keep(1, "aaaaaa");
    keep(2, "bbbbbb");
    leader(10, 1, "aaaaaa");
    leader(11, 2, "bbbbbb");
    keep(3, "cccccc") /* placed at the start, up to the report of 2 */;
    keep(4, "d") /* no room between the report of 3 and the report of 2 */;
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].first, 4);
    EXPECT_EQ(links_.stopped[0].second, Reason::KeptReportsFull);
    EXPECT_EQ(comparison_->kept(), 2U);
    EXPECT_EQ(take_after(1), (std::vector<std::string>{"bbbbbb", "cccccc"}));
}

TEST_F(FollowerReportComparisonTest, AReportLargerThanTheWholeStoreRefusesTheLead) {
    use_a_store_of(10, 16);
    comparison_->start_keeping_after(0);
    keep(1, "seventeen bytes!!");
    ASSERT_EQ(links_.stopped.size(), 1U);
    EXPECT_EQ(links_.stopped[0].second, Reason::KeptReportsFull);
    EXPECT_EQ(comparison_->kept(), 0U);
    leader(10, 1, "seventeen bytes!!");
    EXPECT_EQ(comparison_->differences(), 0) << "a report not kept for want of room is no difference";
}

TEST_F(FollowerReportComparisonTest, AReportThatCouldNotBeWrittenIsNotKept) {
    comparison_->start_keeping_after(0);
    comparison_->keep(1, 1, fix_common::SessionIdentity{}, 8, [](uint8_t*, size_t) -> size_t { return 0; });
    EXPECT_EQ(comparison_->kept(), 0U);
    EXPECT_FALSE(comparison_->refused_to_lead());
}

TEST_F(FollowerReportComparisonTest, TheStoreIsLockedIntoMemoryAndStillKeepsReports) {
    use_a_store_of(1000, 64 * 1024);
    EXPECT_GE(comparison_->bytes_to_hold_in_memory(), 64U * 1024U + 1000U);
    ASSERT_EQ(comparison_->hold_in_memory(), 0);
    comparison_->start_keeping_after(0);
    keep(1, "accepted");
    leader(10, 1, "accepted");
    EXPECT_EQ(comparison_->differences(), 0);
}

TEST_F(FollowerReportComparisonTest, AStoreLargerThanTheLimitOnLockedMemoryIsNotLocked) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "a process running as root may lock memory whatever its limit";
    }
    use_a_store_of(1000, 4 * 1024 * 1024);
    const LockedMemoryLimitLowered lowered(64 * 1024);
    EXPECT_NE(comparison_->hold_in_memory(), 0);
}

TEST_F(FollowerReportComparisonTest, AStoreForAnEngineThatNeverFollowsHasNothingToLock) {
    use_a_store_of(0, 0);
    EXPECT_EQ(comparison_->bytes_to_hold_in_memory(), 0U);
    EXPECT_EQ(comparison_->hold_in_memory(), 0);
}

} // namespaces
