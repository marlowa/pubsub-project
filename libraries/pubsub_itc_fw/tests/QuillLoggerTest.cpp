// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <thread>
#include <chrono>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <quill/Backend.h>
#include <quill/Frontend.h>

#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/tests_common/TestSink.hpp>

using namespace pubsub_itc_fw;

/**
 * Test fixture for QuillLogger tests
 */
class QuillLoggerTest : public ::testing::Test {
protected:

    QuillLoggerTest() {
        // Generate a unique name for this specific test case
        const std::string unique_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();

        test_sink_ = quill::Frontend::create_or_get_sink<TestSink>("quill_logger_test_sink");
        test_sink_ptr_ = static_cast<TestSink*>(test_sink_.get());
        test_sink_->set_log_level_filter(quill::LogLevel::TraceL3);
        test_sink_ptr_->clear();

        // This forces Quill to create a brand new logger attached to YOUR sink
        logger_ = std::make_unique<QuillLogger>(unique_name, test_sink_, FwLogLevel::Debug);
    }

    void SetUp() override {
        // Clear any previous records
        test_sink_ptr_->clear();
    }

    std::shared_ptr<quill::Sink> test_sink_;
    TestSink* test_sink_ptr_{nullptr};
    std::unique_ptr<QuillLogger> logger_;

};

// =============================================================================
// Basic Logging Tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsDebugMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Debug message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Debug), 1);
}

TEST_F(QuillLoggerTest, LogsInfoMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Info), 1);
}

TEST_F(QuillLoggerTest, LogsWarningMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Warning), 1);
}

TEST_F(QuillLoggerTest, LogsErrorMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Error);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error, "Error message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Error), 1);
}

TEST_F(QuillLoggerTest, LogsCriticalMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Critical);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Critical, "Critical message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Critical message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Critical), 1);
}

TEST_F(QuillLoggerTest, LogsAlertMessage) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Alert);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Alert, "Alert message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Alert message"));
    // Alert maps to Critical in Quill
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Critical), 1);
}

// =============================================================================
// Formatted Message Tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsFormattedMessageWithInteger) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);
    [[maybe_unused]] const int value = 42; // not really unused but in clang-tidy we neutralise the logging macros

    // Act
    PUBSUB_LOG(*logger_, FwLogLevel::Info, "Value is {}", value);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Value is 42"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithString) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);
    [[maybe_unused]] const std::string name = "Alice"; // not unused but logger macros neutralised for clang-tidy

    // Act
    PUBSUB_LOG(*logger_, FwLogLevel::Info, "Hello {}", name);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Hello Alice"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithMultipleArgs) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    PUBSUB_LOG(*logger_, FwLogLevel::Info, "User {} has {} points", "Bob", 100);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("User Bob has 100 points"));
}

TEST_F(QuillLoggerTest, LogsStringWithPubSubLogStr) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Simple string message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Simple string message"));
}

// =============================================================================
// Log Level Filtering Tests
// =============================================================================

TEST_F(QuillLoggerTest, FiltersDebugWhenLevelIsInfo) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug message - should be filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info message - should appear");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_FALSE(test_sink_ptr_->contains_message("Debug message"));
}

TEST_F(QuillLoggerTest, FiltersInfoWhenLevelIsWarning) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning"));
}

TEST_F(QuillLoggerTest, FiltersWarningWhenLevelIsError) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Error);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error, "Error - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error"));
}

TEST_F(QuillLoggerTest, LogsAllLevelsWhenSetToDebug) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Warning, "Warning");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Error, "Error");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Critical, "Critical");

    // Assert
    EXPECT_EQ(test_sink_ptr_->count(), 5);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Debug"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Critical"));
}

// =============================================================================
// Dynamic Log Level Change Tests
// =============================================================================

TEST_F(QuillLoggerTest, ChangesLogLevelDynamically) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act - first log at Info level
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug 1 - filtered");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info 1 - appears");

    // Change to Debug level
    logger_->set_log_level(FwLogLevel::Debug);

    PUBSUB_LOG_STR(*logger_, FwLogLevel::Debug, "Debug 2 - appears");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Info 2 - appears");

    // Assert
    EXPECT_EQ(test_sink_ptr_->count(), 3);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info 1"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Debug 2"));
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info 2"));
    EXPECT_FALSE(test_sink_ptr_->contains_message("Debug 1"));
}

// =============================================================================
// Multiple Messages Tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsMultipleMessagesInSequence) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 1");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 2");
    PUBSUB_LOG_STR(*logger_, FwLogLevel::Info, "Message 3");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 3);
    const auto& records = test_sink_ptr_->records();
    EXPECT_TRUE(records[0].message.find("Message 1") != std::string::npos);
    EXPECT_TRUE(records[1].message.find("Message 2") != std::string::npos);
    EXPECT_TRUE(records[2].message.find("Message 3") != std::string::npos);
}

// =============================================================================
// should_log_to_* Method Tests
// =============================================================================

TEST_F(QuillLoggerTest, ShouldLogToFileRespectsLevel) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_file(FwLogLevel::Error));
    EXPECT_TRUE(logger_->should_log_to_file(FwLogLevel::Warning));
    EXPECT_TRUE(logger_->should_log_to_file(FwLogLevel::Info));
    EXPECT_FALSE(logger_->should_log_to_file(FwLogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToConsoleRespectsLevel) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Warning);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_console(FwLogLevel::Critical));
    EXPECT_TRUE(logger_->should_log_to_console(FwLogLevel::Error));
    EXPECT_TRUE(logger_->should_log_to_console(FwLogLevel::Warning));
    EXPECT_FALSE(logger_->should_log_to_console(FwLogLevel::Info));
    EXPECT_FALSE(logger_->should_log_to_console(FwLogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToSyslogRespectsLevel) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Error);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_syslog(FwLogLevel::Critical));
    EXPECT_TRUE(logger_->should_log_to_syslog(FwLogLevel::Error));
    EXPECT_FALSE(logger_->should_log_to_syslog(FwLogLevel::Warning));
    EXPECT_FALSE(logger_->should_log_to_syslog(FwLogLevel::Info));
}

// =============================================================================
// Log Level Getter Test
// =============================================================================

TEST_F(QuillLoggerTest, ReturnsCorrectLogLevel) {
    // Arrange & Act
    logger_->set_log_level(FwLogLevel::Warning);

    // Assert
    EXPECT_EQ(logger_->log_level(), FwLogLevel::Warning);
}

TEST_F(QuillLoggerTest, ReturnsUpdatedLogLevelAfterChange) {
    // Arrange
    logger_->set_log_level(FwLogLevel::Info);

    // Act
    logger_->set_log_level(FwLogLevel::Error);

    // Assert
    EXPECT_EQ(logger_->log_level(), FwLogLevel::Error);
}

// =============================================================================
// Logger Isolation Test
// =============================================================================

TEST(QuillLoggerIsolationTest, TwoLoggersWithSeparateSinksDoNotCrossTalk) {
    // Purpose: Verify that two QuillLogger instances with unique names
    //          and unique sinks do not leak messages to each other.
    //
    // This test ensures that logger isolation works correctly when
    // creating multiple loggers with different sinks.

    // Create first logger with unique sink
    auto sink1 = quill::Frontend::create_or_get_sink<TestSink>("isolation_test_sink_1");
    auto* test_sink1 = static_cast<TestSink*>(sink1.get());
    test_sink1->clear();

    const QuillLogger logger1("isolation_logger_1", sink1, FwLogLevel::Debug);

    // Create second logger with unique sink
    auto sink2 = quill::Frontend::create_or_get_sink<TestSink>("isolation_test_sink_2");
    auto* test_sink2 = static_cast<TestSink*>(sink2.get());
    test_sink2->clear();

    const QuillLogger logger2("isolation_logger_2", sink2, FwLogLevel::Debug);

    // Log unique messages to each logger
    PUBSUB_LOG_STR(logger1, FwLogLevel::Info, "Message from logger1");
    PUBSUB_LOG_STR(logger2, FwLogLevel::Info, "Message from logger2");

    // Verify each sink only contains its own message
    EXPECT_EQ(test_sink1->count(), 1);
    EXPECT_EQ(test_sink2->count(), 1);

    EXPECT_TRUE(test_sink1->contains_message("Message from logger1"));
    EXPECT_FALSE(test_sink1->contains_message("Message from logger2"));

    EXPECT_TRUE(test_sink2->contains_message("Message from logger2"));
    EXPECT_FALSE(test_sink2->contains_message("Message from logger1"));
}

TEST(QuillLoggerIsolationTest, TwoLoggersWithSameSinkNameShareSink) {
    // Purpose: Demonstrate that reusing sink names causes sink sharing.
    //
    // This is a NEGATIVE test showing what happens when you reuse
    // sink names - both loggers will write to the same sink.

    // Create first logger
    auto sink1 = quill::Frontend::create_or_get_sink<TestSink>("shared_sink_name");
    auto* test_sink1 = static_cast<TestSink*>(sink1.get());
    test_sink1->clear();

    const QuillLogger logger1("shared_test_logger_1", sink1, FwLogLevel::Debug);

    // Create second logger with SAME sink name - it will get the same sink!
    auto sink2 = quill::Frontend::create_or_get_sink<TestSink>("shared_sink_name");
    auto* test_sink2 = static_cast<TestSink*>(sink2.get());

    const QuillLogger logger2("shared_test_logger_2", sink2, FwLogLevel::Debug);

    // Verify they're the same sink instance
    EXPECT_EQ(sink1.get(), sink2.get());
    EXPECT_EQ(test_sink1, test_sink2);

    // Log to both loggers
    PUBSUB_LOG_STR(logger1, FwLogLevel::Info, "From logger1");
    PUBSUB_LOG_STR(logger2, FwLogLevel::Info, "From logger2");

    // Both messages appear in the SAME sink (because it's the same instance)
    EXPECT_EQ(test_sink1->count(), 2);
    EXPECT_TRUE(test_sink1->contains_message("From logger1"));
    EXPECT_TRUE(test_sink1->contains_message("From logger2"));
}

TEST(QuillLoggerIsolationTest, TwoLoggersWithSameLoggerNameShareLogger) {
    // Purpose: Demonstrate that Quill's create_or_get_logger returns
    //          the same logger instance when given the same name.
    //
    // This is the root cause of the ApplicationThreadTest bug.

    auto sink1 = quill::Frontend::create_or_get_sink<TestSink>("logger_name_test_sink1");
    auto* test_sink1 = static_cast<TestSink*>(sink1.get());
    test_sink1->clear();

    auto sink2 = quill::Frontend::create_or_get_sink<TestSink>("logger_name_test_sink2");
    auto* test_sink2 = static_cast<TestSink*>(sink2.get());
    test_sink2->clear();

    // Create first logger with name "duplicate_name" and sink1
    const QuillLogger logger1("duplicate_name", sink1, FwLogLevel::Debug);

    // Create second logger with SAME name but different sink
    // QuillLogger constructor calls create_or_get_logger("duplicate_name", {sink2})
    // but Quill will return the EXISTING logger with sink1!
    const QuillLogger logger2("duplicate_name", sink2, FwLogLevel::Debug);

    // Verify both QuillLogger instances point to the same underlying quill::Logger
    EXPECT_EQ(logger1.quill_logger(), logger2.quill_logger());

    // Log to logger2 - but it goes to logger1's sink!
    PUBSUB_LOG_STR(logger2, FwLogLevel::Info, "Logged via logger2");

    // The message appears in sink1 (not sink2) because both loggers share the underlying Quill logger
    EXPECT_TRUE(test_sink1->contains_message("Logged via logger2"));

    // sink2 is empty because logger2 never actually used it
    EXPECT_EQ(test_sink2->count(), 0);
}

// =============================================================================
// Default Constructor Isolation Test
// =============================================================================

TEST(QuillLoggerDefaultConstructorTest, MultipleInstancesShareLoggerAndSink) {
    const QuillLogger logger1;
    const QuillLogger logger2;

    EXPECT_NE(logger1.quill_logger(), logger2.quill_logger())
        << "Expected both loggers to have separate underlying Quill loggers "
        << "due to unique name generation";
}

// =============================================================================
// File Constructor Isolation Test
// =============================================================================

TEST(QuillLoggerFileConstructorTest, MultipleInstancesShareLoggerAndSink) {
    const std::string file1 = "/tmp/quill_test_log1.log";
    const std::string file2 = "/tmp/quill_test_log2.log";

    const QuillLogger logger1(file1, FileOpenMode(FileOpenMode::Truncate), FwLogLevel::Debug, FwLogLevel::Debug, FwLogLevel::Debug);
    const QuillLogger logger2(file2, FileOpenMode(FileOpenMode::Truncate), FwLogLevel::Debug, FwLogLevel::Debug, FwLogLevel::Debug);

    // After fix: loggers should be SEPARATE
    EXPECT_NE(logger1.quill_logger(), logger2.quill_logger())
        << "Expected both loggers to have separate underlying Quill loggers "
        << "due to unique name generation in file constructor (fix applied)";

    (void)std::remove(file1.c_str()); // we don't care if these removes fail
    (void)std::remove(file2.c_str());
}

// =============================================================================
// Cross-Talk Demonstration Test
// =============================================================================

TEST(QuillLoggerDefaultConstructorTest, LoggingCrossTalkBetweenInstances) {
    QuillLogger logger1;
    QuillLogger logger2;

    logger1.set_log_level(FwLogLevel::Error);

    // After fix: logger2 should NOT be affected by logger1's change
    EXPECT_NE(logger2.log_level(), FwLogLevel(FwLogLevel::Error))
        << "Expected logger2's level to remain independent from logger1";

    logger2.set_log_level(FwLogLevel::Debug);

    // After fix: logger1 should maintain its own level
    EXPECT_EQ(logger1.log_level(), FwLogLevel::Error)
        << "Expected logger1's level to remain Error (not changed by logger2)";
}

// =============================================================================
// Sink Sharing Test
// =============================================================================

TEST(QuillLoggerDefaultConstructorTest, MultipleInstancesShareConsoleSink) {
    const QuillLogger logger1;
    const QuillLogger logger2;

    // After fix: loggers should be separate (sink separation follows)
    EXPECT_NE(logger1.quill_logger(), logger2.quill_logger())
        << "Logger independence confirmed - each has unique sinks";
}

// =============================================================================
// Production Scenario Test
// =============================================================================

TEST(QuillLoggerIsolationTest, ProductionScenarioWithMultipleComponents) {
    // Purpose: Simulate a production scenario where multiple components
    //          each create their own QuillLogger instance expecting isolation.
    //
    // This verifies that the fix ensures proper logger independence.

    // Component A creates its logger
    QuillLogger component_a_logger;
    component_a_logger.set_log_level(FwLogLevel::Error);

    // Component B creates its logger, expecting it to be independent
    QuillLogger component_b_logger;
    component_b_logger.set_log_level(FwLogLevel::Debug);

    // After fix: Component A should maintain its own Error level
    EXPECT_EQ(component_a_logger.log_level(), FwLogLevel::Error)
        << "Component A's log level should remain Error (independent from Component B)";

    // After fix: Component B should have its own Debug level
    EXPECT_EQ(component_b_logger.log_level(), FwLogLevel::Debug)
        << "Component B's log level should remain Debug (independent from Component A)";
}

// =============================================================================
// Recommended Fix Verification Test (for future implementation)
// =============================================================================

TEST(QuillLoggerIsolationTest, ShouldProvideIsolation) {
    // Purpose: This test is DISABLED and shows what SHOULD happen after fixing
    //          the hardcoded names issue.
    //
    // Enable this test after implementing unique name generation.

    QuillLogger logger1;
    QuillLogger logger2;

    // After fix: These should be DIFFERENT logger instances
    EXPECT_NE(logger1.quill_logger(), logger2.quill_logger())
        << "After fix, each QuillLogger instance should have its own "
        << "unique underlying Quill logger";

    // After fix: Setting one logger's level shouldn't affect the other
    logger1.set_log_level(FwLogLevel::Error);
    logger2.set_log_level(FwLogLevel::Debug);

    EXPECT_EQ(logger1.log_level(), FwLogLevel::Error);
    EXPECT_EQ(logger2.log_level(), FwLogLevel::Debug);
}

TEST(QuillLoggerSyslogTest, FileConstructorEnablesSyslogFiltering) {
    const std::string temp_file = "/tmp/test_syslog_config.log";

    // Create logger with different syslog level
    const QuillLogger logger(temp_file,
                       FileOpenMode(FileOpenMode::Truncate),
                       FwLogLevel::Debug,   // file logs everything
                       FwLogLevel::Error,   // syslog only errors
                       FwLogLevel::Info);   // console logs info+

    // Verify filtering configuration
    EXPECT_TRUE(logger.should_log_to_file(FwLogLevel::Debug));
    EXPECT_TRUE(logger.should_log_to_syslog(FwLogLevel::Error));
    EXPECT_FALSE(logger.should_log_to_syslog(FwLogLevel::Warning));
    EXPECT_FALSE(logger.should_log_to_syslog(FwLogLevel::Info));

    // Verify logger was created successfully
    EXPECT_NE(logger.quill_logger(), nullptr);

    (void) std::remove(temp_file.c_str()); // we don't care if this remove fail
}

TEST(QuillLoggerSyslogTest, SyslogLevelIndependentFromOtherSinks) {
    const std::string temp_file = "/tmp/test_syslog_independence.log";

    const QuillLogger logger(temp_file,
                       FileOpenMode(FileOpenMode::Truncate),
                       FwLogLevel::Debug,     // file
                       FwLogLevel::Critical,  // syslog - most restrictive
                       FwLogLevel::Warning);  // console

    EXPECT_TRUE(logger.should_log_to_file(FwLogLevel::Debug));
    EXPECT_TRUE(logger.should_log_to_console(FwLogLevel::Warning));
    EXPECT_FALSE(logger.should_log_to_console(FwLogLevel::Debug));
    EXPECT_TRUE(logger.should_log_to_syslog(FwLogLevel::Critical));
    EXPECT_FALSE(logger.should_log_to_syslog(FwLogLevel::Error));

    (void)std::remove(temp_file.c_str()); // we don't care if this remove fail
}

TEST(QuillLoggerSignalTest, BlockSignalsBeforeConstructionDoesNotThrow) {
    // block_signals_before_construction() is intended to be called before
    // constructing a QuillLogger on the main thread so that SIGTERM and SIGINT
    // are blocked. It must not throw or crash.
    QuillLogger logger;
    EXPECT_NO_THROW(logger.block_signals_before_construction());
}
