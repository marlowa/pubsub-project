// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/LastStateFile.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace pubsub_itc_fw::tests {

namespace {

class LastStateFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = tests_common::make_scratch_directory("last_state_file_test");
        path_ = directory_ + "/sequencer_primary.last_state";
    }
    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    std::string directory_;
    std::string path_;
};

} // namespaces

TEST_F(LastStateFileTest, ThePhaseRecordedIsReadBackWithItsTradingDay) {
    const LastStateFile file(path_, 20'261'119);
    file.record("Open");
    const std::optional<LastStateFile::Recorded> recorded = LastStateFile::read(path_);
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(recorded->trading_day, 20'261'119);
    EXPECT_EQ(recorded->phase, "Open");
    std::ifstream text(path_);
    std::string line;
    std::getline(text, line);
    EXPECT_EQ(line, "trading_day=20261119 Open") << "the form the design gives, readable by an operator";
}

TEST_F(LastStateFileTest, EachPhaseReplacesTheLastAndLeavesNoTemporaryFile) {
    const LastStateFile file(path_, 20'261'119);
    file.record("EndOfDayBegin");
    file.record("EndOfDayEnd");
    EXPECT_EQ(LastStateFile::read(path_)->phase, "EndOfDayEnd");
    EXPECT_FALSE(std::filesystem::exists(path_ + ".tmp"));
}

// A component started for its day finds the end of that day, and only that day, in the file.
TEST_F(LastStateFileTest, TheEndOfDayCountsOnlyForTheComponentsOwnDay) {
    LastStateFile(path_, 20'261'118).record("EndOfDayEnd");
    EXPECT_TRUE(LastStateFile(path_, 20'261'118).says("EndOfDayEnd"));
    EXPECT_FALSE(LastStateFile(path_, 20'261'119).says("EndOfDayEnd")) << "yesterday's end is not today's";
    EXPECT_FALSE(LastStateFile(path_, 20'261'118).says("Open"));
}

TEST_F(LastStateFileTest, NoFileOrAFileWithoutADayOrPhaseSaysNothing) {
    EXPECT_FALSE(LastStateFile::read(path_).has_value());
    EXPECT_FALSE(LastStateFile(path_, 20'261'119).says("EndOfDayEnd"));
    std::ofstream(path_) << "EndOfDayEnd\n";
    EXPECT_FALSE(LastStateFile::read(path_).has_value()) << "no trading day";
    std::ofstream(path_) << "trading_day=20261119\n";
    EXPECT_FALSE(LastStateFile::read(path_).has_value()) << "no phase";
}

TEST_F(LastStateFileTest, AnEmptyPathKeepsNoFile) {
    const LastStateFile file("", 20'261'119);
    file.record("Open");
    EXPECT_FALSE(file.says("Open"));
    EXPECT_TRUE(std::filesystem::is_empty(directory_));
}

TEST_F(LastStateFileTest, AFileThatCannotBeWrittenIsAnError) {
    const LastStateFile file(directory_ + "/no_such_directory/component.last_state", 20'261'119);
    EXPECT_THROW(file.record("Open"), PubSubItcException);
}

} // namespaces
