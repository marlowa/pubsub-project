// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

using namespace pubsub_itc_fw;

// =============================================================================
// Test fixture
// =============================================================================

/* QuillLoggerTest
 *
 * Each test gets a fresh unit-test-mode QuillLogger wired to a callback that
 * appends fully formatted records to records_.  The logger threshold starts at
 * Debug so all severities are exercised unless a test overrides it.
 */
class QuillLoggerTest : public ::testing::Test {
protected:
    QuillLoggerTest() {
        logger_ = std::make_unique<QuillLogger>(
            FwLogLevel::Debug,
            [this](const std::string& record) {
                records_.push_back(record);
            });
    }

    void SetUp() override {
        records_.clear();
    }

    bool contains_message(const std::string& text) const {
        for (const auto& record : records_) {
            if (record.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> records_;
    std::unique_ptr<QuillLogger> logger_;
};

// =============================================================================
// Basic logging tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsDebugMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Debug message"));
}

TEST_F(QuillLoggerTest, LogsInfoMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Info message"));
}

TEST_F(QuillLoggerTest, LogsWarningMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Warning message"));
}

TEST_F(QuillLoggerTest, LogsErrorMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error, "Error message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Error message"));
}

TEST_F(QuillLoggerTest, LogsCriticalMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Critical, "Critical message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Critical message"));
}

TEST_F(QuillLoggerTest, LogsAlertMessage) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Alert, "Alert message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Alert message"));
}

// =============================================================================
// Formatted message tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsFormattedMessageWithInteger) {
    [[maybe_unused]] const int value = 42;

    PUBSUB_LOG(*logger_, FwLogLevel::Info, "Value is {}", value);

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Value is 42"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithString) {
    [[maybe_unused]] const std::string name = "Alice";

    PUBSUB_LOG(*logger_, FwLogLevel::Info, "Hello {}", name);

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Hello Alice"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithMultipleArgs) {
    PUBSUB_LOG(*logger_, FwLogLevel::Info, "User {} has {} points", "Bob", 100);

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("User Bob has 100 points"));
}

TEST_F(QuillLoggerTest, LogsStringWithPubSubLogStr) {
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Simple string message");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Simple string message"));
}

// =============================================================================
// Log level filtering tests
// =============================================================================

TEST_F(QuillLoggerTest, FiltersDebugWhenLevelIsInfo) {
    logger_->set_log_level(FwLogLevel::Info);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug message - should be filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,  "Info message - should appear");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Info message"));
    EXPECT_FALSE(contains_message("Debug message"));
}

TEST_F(QuillLoggerTest, FiltersInfoWhenLevelIsWarning) {
    logger_->set_log_level(FwLogLevel::Warning);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,    "Info - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning - appears");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Warning"));
}

TEST_F(QuillLoggerTest, FiltersWarningWhenLevelIsError) {
    logger_->set_log_level(FwLogLevel::Error);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error,   "Error - appears");

    ASSERT_EQ(records_.size(), 1u);
    EXPECT_TRUE(contains_message("Error"));
}

TEST_F(QuillLoggerTest, LogsAllLevelsWhenSetToDebug) {
    logger_->set_log_level(FwLogLevel::Debug);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug,    "Debug");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,     "Info");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning,  "Warning");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error,    "Error");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Critical, "Critical");

    EXPECT_EQ(records_.size(), 5u);
    EXPECT_TRUE(contains_message("Debug"));
    EXPECT_TRUE(contains_message("Info"));
    EXPECT_TRUE(contains_message("Warning"));
    EXPECT_TRUE(contains_message("Error"));
    EXPECT_TRUE(contains_message("Critical"));
}

TEST_F(QuillLoggerTest, SuppressedMessageProducesNoCallback) {
    logger_->set_log_level(FwLogLevel::Error);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug,   "Debug - suppressed");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,    "Info - suppressed");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning - suppressed");

    EXPECT_EQ(records_.size(), 0u);
}

// =============================================================================
// Dynamic log level change tests
// =============================================================================

TEST_F(QuillLoggerTest, ChangesLogLevelDynamically) {
    logger_->set_log_level(FwLogLevel::Info);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug 1 - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,  "Info 1 - appears");

    logger_->set_log_level(FwLogLevel::Debug);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug 2 - appears");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info,  "Info 2 - appears");

    EXPECT_EQ(records_.size(), 3u);
    EXPECT_TRUE(contains_message("Info 1"));
    EXPECT_TRUE(contains_message("Debug 2"));
    EXPECT_TRUE(contains_message("Info 2"));
    EXPECT_FALSE(contains_message("Debug 1"));
}

// =============================================================================
// Multiple messages tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsMultipleMessagesInSequence) {
    logger_->set_log_level(FwLogLevel::Info);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 1");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 2");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 3");

    ASSERT_EQ(records_.size(), 3u);
    EXPECT_TRUE(records_[0].find("Message 1") != std::string::npos);
    EXPECT_TRUE(records_[1].find("Message 2") != std::string::npos);
    EXPECT_TRUE(records_[2].find("Message 3") != std::string::npos);
}

// =============================================================================
// Log level getter tests
// =============================================================================

TEST_F(QuillLoggerTest, ReturnsCorrectLogLevel) {
    logger_->set_log_level(FwLogLevel::Warning);

    EXPECT_EQ(logger_->log_level(), FwLogLevel::Warning);
}

TEST_F(QuillLoggerTest, ReturnsUpdatedLogLevelAfterChange) {
    logger_->set_log_level(FwLogLevel::Info);
    logger_->set_log_level(FwLogLevel::Error);

    EXPECT_EQ(logger_->log_level(), FwLogLevel::Error);
}

// =============================================================================
// Logger isolation tests
// =============================================================================

TEST(QuillLoggerIsolationTest, TwoLoggersDoNotCrossTalk) {
    std::vector<std::string> records1;
    std::vector<std::string> records2;

    QuillLogger logger1(FwLogLevel::Debug, [&records1](const std::string& r) { records1.push_back(r); });
    QuillLogger logger2(FwLogLevel::Debug, [&records2](const std::string& r) { records2.push_back(r); });

    PUBSUB_LOG_STR(logger1, FwLogLevel::Info, "Message from logger1");
    PUBSUB_LOG_STR(logger2, FwLogLevel::Info, "Message from logger2");

    ASSERT_EQ(records1.size(), 1u);
    ASSERT_EQ(records2.size(), 1u);

    EXPECT_TRUE(records1[0].find("Message from logger1") != std::string::npos);
    EXPECT_TRUE(records2[0].find("Message from logger2") != std::string::npos);

    EXPECT_NE(logger1.quill_logger(), logger2.quill_logger())
        << "Two separate QuillLogger instances must have separate underlying Quill loggers";
}

TEST(QuillLoggerIsolationTest, LevelChangeOnOneLoggerDoesNotAffectOther) {
    QuillLogger logger1(FwLogLevel::Info,  nullptr);
    QuillLogger logger2(FwLogLevel::Debug, nullptr);

    logger1.set_log_level(FwLogLevel::Error);

    EXPECT_EQ(logger1.log_level(), FwLogLevel::Error);
    EXPECT_EQ(logger2.log_level(), FwLogLevel::Debug);
}

// =============================================================================
// Format string mismatch detection test
// =============================================================================

// Verifies that a mismatch between the format string and the argument list is
// detected by the Quill backend and reported as a log record containing
// "Could not format log statement".
//
// In Quill 11.0.2, format errors are caught inside _populate_formatted_log_message
// and emitted as a normal log record rather than going through error_notifier.
// The error_notifier is only called for queue failures and backend exceptions.
//
// Note: compile-time format string checking is not available in C++17 -- see
// LoggingMacros.hpp for a full explanation. This test verifies the runtime
// fallback. The mismatch is intentional.
TEST_F(QuillLoggerTest, FormatMismatchIsReportedAsLogRecord) {
    // NOLINTNEXTLINE -- deliberate format string mismatch for testing
    PUBSUB_LOG((*logger_), FwLogLevel::Info, "{}:{} intentional mismatch", "only_one_arg");

    // Wait for the backend to process and emit the error record.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (contains_message("Could not format")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(contains_message("Could not format"))
        << "Expected a 'Could not format' error record for format string mismatch";
    EXPECT_TRUE(contains_message("argument not found") || contains_message("intentional mismatch"))
        << "Error record should identify the bad format string";
}

// =============================================================================
// Signal safety test
// =============================================================================

TEST(QuillLoggerSignalTest, BlockSignalsBeforeConstructionDoesNotThrow) {
    EXPECT_NO_THROW(QuillLogger::block_signals_before_construction());
}
