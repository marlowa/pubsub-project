#include <thread>
#include <chrono>

#include <gtest/gtest.h>

#include <quill/Backend.h>
#include <quill/Frontend.h>

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/tests_common/TestSink.hpp>

using namespace pubsub_itc_fw;

/**
 * Test fixture for QuillLogger tests
 */
class QuillLoggerTest : public ::testing::Test {
protected:

    QuillLoggerTest() {
        // Generate a unique name for this specific test case
        std::string unique_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();

        test_sink_ = quill::Frontend::create_or_get_sink<TestSink>("quill_logger_test_sink");
        test_sink_ptr_ = static_cast<TestSink*>(test_sink_.get());
        test_sink_->set_log_level_filter(quill::LogLevel::TraceL3);
        test_sink_ptr_->clear();

        // This forces Quill to create a brand new logger attached to YOUR sink
        logger_ = std::make_unique<QuillLogger>(unique_name, test_sink_, LogLevel::Debug);
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
    logger_->set_log_level(LogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Debug, "Debug message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Debug message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Debug), 1);
}

TEST_F(QuillLoggerTest, LogsInfoMessage) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Info), 1);
}

TEST_F(QuillLoggerTest, LogsWarningMessage) {
    // Arrange
    logger_->set_log_level(LogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Warning, "Warning message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Warning), 1);
}

TEST_F(QuillLoggerTest, LogsErrorMessage) {
    // Arrange
    logger_->set_log_level(LogLevel::Error);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Error, "Error message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Error), 1);
}

TEST_F(QuillLoggerTest, LogsCriticalMessage) {
    // Arrange
    logger_->set_log_level(LogLevel::Critical);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Critical, "Critical message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Critical message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Critical), 1);
}

TEST_F(QuillLoggerTest, LogsAlertMessage) {
    // Arrange
    logger_->set_log_level(LogLevel::Alert);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Alert, "Alert message");

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
    logger_->set_log_level(LogLevel::Info);
    int value = 42;

    // Act
    PUBSUB_LOG(*logger_, LogLevel::Info, "Value is {}", value);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Value is 42"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithString) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);
    std::string name = "Alice";

    // Act
    PUBSUB_LOG(*logger_, LogLevel::Info, "Hello {}", name);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Hello Alice"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithMultipleArgs) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);

    // Act
    PUBSUB_LOG(*logger_, LogLevel::Info, "User {} has {} points", "Bob", 100);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("User Bob has 100 points"));
}

TEST_F(QuillLoggerTest, LogsStringWithPUBSUB_LOG_STR) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Simple string message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Simple string message"));
}

// =============================================================================
// Log Level Filtering Tests
// =============================================================================

TEST_F(QuillLoggerTest, FiltersDebugWhenLevelIsInfo) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Debug, "Debug message - should be filtered");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info message - should appear");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_FALSE(test_sink_ptr_->contains_message("Debug message"));
}

TEST_F(QuillLoggerTest, FiltersInfoWhenLevelIsWarning) {
    // Arrange
    logger_->set_log_level(LogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info - filtered");
    PUBSUB_LOG_STR(*logger_, LogLevel::Warning, "Warning - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning"));
}

TEST_F(QuillLoggerTest, FiltersWarningWhenLevelIsError) {
    // Arrange
    logger_->set_log_level(LogLevel::Error);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Warning, "Warning - filtered");
    PUBSUB_LOG_STR(*logger_, LogLevel::Error, "Error - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error"));
}

TEST_F(QuillLoggerTest, LogsAllLevelsWhenSetToDebug) {
    // Arrange
    logger_->set_log_level(LogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Debug, "Debug");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info");
    PUBSUB_LOG_STR(*logger_, LogLevel::Warning, "Warning");
    PUBSUB_LOG_STR(*logger_, LogLevel::Error, "Error");
    PUBSUB_LOG_STR(*logger_, LogLevel::Critical, "Critical");

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
    logger_->set_log_level(LogLevel::Info);

    // Act - first log at Info level
    PUBSUB_LOG_STR(*logger_, LogLevel::Debug, "Debug 1 - filtered");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info 1 - appears");

    // Change to Debug level
    logger_->set_log_level(LogLevel::Debug);

    PUBSUB_LOG_STR(*logger_, LogLevel::Debug, "Debug 2 - appears");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Info 2 - appears");

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
    logger_->set_log_level(LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Message 1");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Message 2");
    PUBSUB_LOG_STR(*logger_, LogLevel::Info, "Message 3");

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
    logger_->set_log_level(LogLevel::Info);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_file(LogLevel::Error));
    EXPECT_TRUE(logger_->should_log_to_file(LogLevel::Warning));
    EXPECT_TRUE(logger_->should_log_to_file(LogLevel::Info));
    EXPECT_FALSE(logger_->should_log_to_file(LogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToConsoleRespectsLevel) {
    // Arrange
    logger_->set_log_level(LogLevel::Warning);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_console(LogLevel::Critical));
    EXPECT_TRUE(logger_->should_log_to_console(LogLevel::Error));
    EXPECT_TRUE(logger_->should_log_to_console(LogLevel::Warning));
    EXPECT_FALSE(logger_->should_log_to_console(LogLevel::Info));
    EXPECT_FALSE(logger_->should_log_to_console(LogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToSyslogRespectsLevel) {
    // Arrange
    logger_->set_log_level(LogLevel::Error);

    // Assert
    EXPECT_TRUE(logger_->should_log_to_syslog(LogLevel::Critical));
    EXPECT_TRUE(logger_->should_log_to_syslog(LogLevel::Error));
    EXPECT_FALSE(logger_->should_log_to_syslog(LogLevel::Warning));
    EXPECT_FALSE(logger_->should_log_to_syslog(LogLevel::Info));
}

// =============================================================================
// Log Level Getter Test
// =============================================================================

TEST_F(QuillLoggerTest, ReturnsCorrectLogLevel) {
    // Arrange & Act
    logger_->set_log_level(LogLevel::Warning);

    // Assert
    EXPECT_EQ(logger_->log_level(), LogLevel::Warning);
}

TEST_F(QuillLoggerTest, ReturnsUpdatedLogLevelAfterChange) {
    // Arrange
    logger_->set_log_level(LogLevel::Info);

    // Act
    logger_->set_log_level(LogLevel::Error);

    // Assert
    EXPECT_EQ(logger_->log_level(), LogLevel::Error);
}
