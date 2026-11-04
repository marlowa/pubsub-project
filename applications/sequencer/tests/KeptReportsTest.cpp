// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "KeptReports.hpp"

namespace sequencer::tests {

namespace {

using Clock = KeptReports::Clock;
using std::chrono::milliseconds;

constexpr Clock::duration lease_period = milliseconds{3000};
constexpr Clock::duration drift_allowance = milliseconds{250};

// What KeptReports asks the thread to do.
struct RecordingLinks : public KeptReportsLinksInterface {
    void forward_kept_report(int64_t er_seq_no, const uint8_t*, size_t) override {
        forwarded.push_back(er_seq_no);
    }
    void kept_reports_lost(int64_t total) override {
        lost = total;
    }
    void kept_reports_forwarded(int64_t discarded, int64_t count, int64_t) override {
        discarded_as_forwarded = discarded;
        forwarded_count = count;
    }
    std::vector<int64_t> forwarded;
    int64_t lost{0};
    int64_t discarded_as_forwarded{-1};
    int64_t forwarded_count{-1};
};

class KeptReportsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        kept_ = std::make_unique<KeptReports>(lease_period, drift_allowance, links_, logger_);
    }

    void keep(int64_t er_seq_no, Clock::time_point at, std::optional<EngineReportPosition> position = std::nullopt) {
        const std::string bytes = "report-" + std::to_string(er_seq_no);
        kept_->keep(er_seq_no, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), position, at);
    }

    static int64_t ns(milliseconds value) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<KeptReports> kept_;
    const Clock::time_point start_ = Clock::now();
};

} // namespaces

TEST_F(KeptReportsTest, EveryKeptReportIsForwardedOnTakingTheLead) {
    keep(1, start_);
    keep(2, start_);
    kept_->forward_all();
    EXPECT_EQ(links_.forwarded, (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(links_.forwarded_count, 2);
    EXPECT_EQ(links_.discarded_as_forwarded, 0);
    EXPECT_EQ(kept_->count(), 0U);
}

TEST_F(KeptReportsTest, AReportFromBeforeTheLeaseTheLeaderWasLastGrantedIsNotNeeded) {
    keep(1, start_);
    keep(3, start_ + milliseconds{5});
    keep(2, start_ + milliseconds{3300});
    kept_->granted_lease_to_leader(start_ + lease_period + drift_allowance + milliseconds{1});
    kept_->forward_all();
    EXPECT_EQ(links_.forwarded, (std::vector<int64_t>{3, 2}))
        << "the leader forwarded everything it received a lease period and the drift allowance before the grant, and only that";
}

TEST_F(KeptReportsTest, ReportsDoNotAgeAgainstTheClockOnlyAgainstGrants) {
    keep(1, start_);
    kept_->forward_all();
    EXPECT_EQ(links_.forwarded, (std::vector<int64_t>{1})) << "no grant yet, so every report may be needed";
}

TEST_F(KeptReportsTest, AReportTheLeaderSaidItForwardedIsDiscardedAfterTheDelay) {
    keep(1, start_, EngineReportPosition{1, 5});
    keep(2, start_, EngineReportPosition{1, 6});
    kept_->leader_record_written(ns(milliseconds{1000}), EngineReportPosition{1, 5});
    kept_->leader_record_written(ns(milliseconds{1050}), std::nullopt);
    kept_->discard_those_the_leader_forwarded();
    EXPECT_EQ(kept_->count(), 2U) << "the record is not yet reports_forwarded_delay older than the latest";
    kept_->leader_record_written(ns(milliseconds{1100}), std::nullopt);
    kept_->discard_those_the_leader_forwarded();
    EXPECT_EQ(kept_->count(), 1U);
    kept_->forward_all();
    EXPECT_EQ(links_.forwarded, (std::vector<int64_t>{2}));
    EXPECT_EQ(links_.discarded_as_forwarded, 1);
}

TEST_F(KeptReportsTest, TheNewestSampleOldEnoughIsTheOneThatApplies) {
    keep(1, start_, EngineReportPosition{1, 5});
    keep(2, start_, EngineReportPosition{1, 6});
    keep(3, start_, EngineReportPosition{1, 7});
    kept_->leader_record_written(ns(milliseconds{1000}), EngineReportPosition{1, 5});
    kept_->leader_record_written(ns(milliseconds{1010}), EngineReportPosition{1, 6});
    kept_->leader_record_written(ns(milliseconds{1150}), EngineReportPosition{1, 7});
    kept_->discard_those_the_leader_forwarded();
    EXPECT_EQ(kept_->count(), 1U) << "through 6, written 140 ms before the latest; through 7 is too recent";
}

TEST_F(KeptReportsTest, TheTimerDiscardsOnceAnInterval) {
    keep(1, start_, EngineReportPosition{1, 5});
    kept_->leader_record_written(ns(milliseconds{1000}), EngineReportPosition{1, 5});
    kept_->leader_record_written(ns(milliseconds{1200}), std::nullopt);
    kept_->tick(start_);
    EXPECT_EQ(kept_->count(), 0U);
    keep(2, start_, EngineReportPosition{1, 5});
    kept_->tick(start_ + milliseconds{500});
    EXPECT_EQ(kept_->count(), 1U) << "not yet a discard interval since the last";
    kept_->tick(start_ + KeptReports::discard_interval);
    EXPECT_EQ(kept_->count(), 0U);
}

TEST_F(KeptReportsTest, AReportLostToAFullStoreIsCounted) {
    for (size_t index = 0; index <= KeptReports::capacity_reports; ++index) {
        keep(static_cast<int64_t>(index), start_);
    }
    EXPECT_EQ(links_.lost, 1);
    EXPECT_EQ(kept_->lost(), 1U);
}

} // namespaces
