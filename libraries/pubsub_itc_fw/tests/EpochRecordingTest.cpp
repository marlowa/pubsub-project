// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <prometheus/gauge.h>

#include <pubsub_itc_fw/EpochRecording.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/SingleWriterHistogram.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace pubsub_itc_fw::tests {

namespace {

using std::chrono::milliseconds;
using Clock = EpochRecording::Clock;

// A lease so short that any record of the epoch takes more than a quarter of it, and one so long that none does.
constexpr milliseconds lease_every_record_is_slow_for{0};
constexpr milliseconds lease_no_record_is_slow_for{1000000};

class EpochRecordingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = tests_common::make_scratch_directory("epoch_recording");
        recording_ = std::make_unique<EpochRecording>(directory_ + "/epoch.state", "TestOwner", logger_);
        recording_->measure_into(HistogramHandle{&record_milliseconds_}, HistogramHandle{&lease_left_milliseconds_});
    }
    void TearDown() override {
        recording_.reset();
        tests_common::remove_scratch_directory(directory_);
    }

    int lines_containing(const std::string& text) {
        logger_.flush();
        int count = 0;
        for (const std::string& line : log_) {
            count += line.find(text) != std::string::npos ? 1 : 0;
        }
        return count;
    }

    std::vector<std::string> log_;
    QuillLogger logger_{FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    std::string directory_;
    SingleWriterHistogram record_milliseconds_{std::vector<double>{1, 10, 100, 1000}};
    SingleWriterHistogram lease_left_milliseconds_{std::vector<double>{1, 10, 100, 1000, 10000}};
    std::unique_ptr<EpochRecording> recording_;
};

} // namespaces

TEST_F(EpochRecordingTest, AnEpochRecordedIsOnDiskAndTimed) {
    ASSERT_TRUE(recording_->record(5, lease_no_record_is_slow_for));
    EXPECT_EQ(recording_->load(), 5);
    EXPECT_EQ(record_milliseconds_.collect().histogram.sample_count, 1U);
    EXPECT_GT(recording_->last_record_took(), Clock::duration::zero()) << "a sync of a file takes some time";
    EXPECT_EQ(lines_containing("more than a quarter of the"), 0) << "a record well within the lease is not warned of";
}

TEST_F(EpochRecordingTest, ARecordThatTakesMoreThanAQuarterOfTheLeaseIsWarnedOfWithTheFileAndTheSetting) {
    ASSERT_TRUE(recording_->record(5, lease_every_record_is_slow_for));
    EXPECT_EQ(lines_containing("more than a quarter of the 0 ms lease"), 1);
    EXPECT_EQ(lines_containing(directory_ + "/epoch.state"), 1);
    EXPECT_EQ(lines_containing("[lease] period_milliseconds"), 1);
}

TEST_F(EpochRecordingTest, TheLeaseLeftOnTakingTheLeadIsRecordedAndNeverBelowZero) {
    const Clock::time_point now = Clock::now();
    recording_->acted_on_taking_the_lead(now + milliseconds{2000}, now);
    recording_->acted_on_taking_the_lead(now - milliseconds{500}, now);
    const prometheus::ClientMetric::Histogram left = lease_left_milliseconds_.collect().histogram;
    EXPECT_EQ(left.sample_count, 2U);
    EXPECT_DOUBLE_EQ(left.sample_sum, 2000.0) << "a lease that ran out before acting counts as none left";
}

TEST_F(EpochRecordingTest, StoppingLeadingAfterASlowRecordOfTheEpochSaysThatIsTheProbableCauseOnce) {
    ASSERT_TRUE(recording_->record(5, lease_every_record_is_slow_for));
    const Clock::time_point now = Clock::now();
    recording_->acted_on_taking_the_lead(now + milliseconds{3000}, now);
    EXPECT_TRUE(recording_->stopped_leading(lease_every_record_is_slow_for));
    EXPECT_EQ(lines_containing("which is the probable cause"), 1);
    EXPECT_FALSE(recording_->stopped_leading(lease_every_record_is_slow_for)) << "said for the leadership it ended, once";
}

TEST_F(EpochRecordingTest, StoppingLeadingIsNotPutDownToARecordThatWasQuick) {
    ASSERT_TRUE(recording_->record(5, lease_no_record_is_slow_for));
    const Clock::time_point now = Clock::now();
    recording_->acted_on_taking_the_lead(now + milliseconds{3000}, now);
    EXPECT_FALSE(recording_->stopped_leading(lease_no_record_is_slow_for));
    EXPECT_EQ(lines_containing("which is the probable cause"), 0);
}

TEST_F(EpochRecordingTest, StoppingLeadingWithoutHavingTakenTheLeadIsNotPutDownToTheRecord) {
    ASSERT_TRUE(recording_->record(5, lease_every_record_is_slow_for));
    EXPECT_FALSE(recording_->stopped_leading(lease_every_record_is_slow_for)) << "the slow record was not the one before leading";
}

TEST_F(EpochRecordingTest, TheDiskCheckMeasuresTrialRecordsSetsTheGaugeAndLeavesNoFileBehind) {
    prometheus::Gauge worst;
    const EpochRecording::DiskCheck found = recording_->check_disk(lease_no_record_is_slow_for, GaugeHandle{&worst});
    EXPECT_GT(found.records, 0);
    EXPECT_LE(found.records, EpochRecording::disk_check_records);
    EXPECT_GT(found.median_milliseconds, 0.0);
    EXPECT_GE(found.worst_milliseconds, found.median_milliseconds);
    EXPECT_DOUBLE_EQ(worst.Value(), found.worst_milliseconds);
    EXPECT_FALSE(std::filesystem::exists(directory_ + "/epoch.state.disk_check")) << "the trial file is removed";
    EXPECT_FALSE(std::filesystem::exists(directory_ + "/epoch.state")) << "the real epoch is not touched";
    EXPECT_EQ(lines_containing("too slow to sync within it"), 0);
    EXPECT_EQ(lines_containing("at the median"), 1);
}

TEST_F(EpochRecordingTest, TheDiskCheckWarnsWhenTheLongestTrialIsMoreThanAQuarterOfTheLease) {
    prometheus::Gauge worst;
    static_cast<void>(recording_->check_disk(lease_every_record_is_slow_for, GaugeHandle{&worst}));
    EXPECT_EQ(lines_containing("too slow to sync within it"), 1);
    EXPECT_EQ(lines_containing("[lease] period_milliseconds"), 1);
}

TEST_F(EpochRecordingTest, WithNoFileForTheEpochTheDiskCheckWritesAndSaysNothing) {
    const std::filesystem::path before = std::filesystem::current_path();
    std::filesystem::current_path(directory_);
    EpochRecording without_a_file("", "TestOwner", logger_);
    prometheus::Gauge worst;
    const EpochRecording::DiskCheck found = without_a_file.check_disk(lease_every_record_is_slow_for, GaugeHandle{&worst});
    const bool trial_left = std::filesystem::exists(directory_ + "/.disk_check") || std::filesystem::exists(directory_ + "/.disk_check.tmp");
    std::filesystem::current_path(before);
    EXPECT_EQ(found.records, 0);
    EXPECT_FALSE(trial_left);
    EXPECT_EQ(lines_containing("trial record"), 0);
    EXPECT_EQ(lines_containing("could not record a trial epoch"), 0);
}

} // namespaces
