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
    void SetUp() override {
        // Create test sink to capture log records
        test_sink_ = quill::Frontend::create_or_get_sink<TestSink>("test_sink");

        // Get raw pointer to TestSink for assertions
        test_sink_ptr_ = static_cast<TestSink*>(test_sink_.get());

        // Clear any previous records
        test_sink_ptr_->clear();
    }

    void TearDown() override {
        // Give Quill backend time to process async logs
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // TODO we do not flush any more due to quill v11 changes
    }

    std::shared_ptr<quill::Sink> test_sink_;
    TestSink* test_sink_ptr_{nullptr};
};

// =============================================================================
// Basic Logging Tests
// =============================================================================

TEST_F(QuillLoggerTest, LogsDebugMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Debug, "Debug message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Debug message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Debug), 1);
}

TEST_F(QuillLoggerTest, LogsInfoMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Info), 1);
}

TEST_F(QuillLoggerTest, LogsWarningMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Warning, "Warning message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Warning), 1);
}

TEST_F(QuillLoggerTest, LogsErrorMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Error);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Error, "Error message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Error), 1);
}

TEST_F(QuillLoggerTest, LogsCriticalMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Critical);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Critical, "Critical message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Critical message"));
    EXPECT_EQ(test_sink_ptr_->count_at_level(quill::LogLevel::Critical), 1);
}

TEST_F(QuillLoggerTest, LogsAlertMessage) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Alert);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Alert, "Alert message");

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
    QuillLogger logger(test_sink_, LogLevel::Info);
    int value = 42;

    // Act
    PUBSUB_LOG(logger, LogLevel::Info, "Value is {}", value);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Value is 42"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithString) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);
    std::string name = "Alice";

    // Act
    PUBSUB_LOG(logger, LogLevel::Info, "Hello {}", name);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Hello Alice"));
}

TEST_F(QuillLoggerTest, LogsFormattedMessageWithMultipleArgs) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    PUBSUB_LOG(logger, LogLevel::Info, "User {} has {} points", "Bob", 100);

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("User Bob has 100 points"));
}

TEST_F(QuillLoggerTest, LogsStringWithPUBSUB_LOG_STR) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Simple string message");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Simple string message"));
}

// =============================================================================
// Log Level Filtering Tests
// =============================================================================

TEST_F(QuillLoggerTest, FiltersDebugWhenLevelIsInfo) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Debug, "Debug message - should be filtered");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info message - should appear");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Info message"));
    EXPECT_FALSE(test_sink_ptr_->contains_message("Debug message"));
}

TEST_F(QuillLoggerTest, FiltersInfoWhenLevelIsWarning) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Warning);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info - filtered");
    PUBSUB_LOG_STR(logger, LogLevel::Warning, "Warning - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Warning"));
}

TEST_F(QuillLoggerTest, FiltersWarningWhenLevelIsError) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Error);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Warning, "Warning - filtered");
    PUBSUB_LOG_STR(logger, LogLevel::Error, "Error - appears");

    // Assert
    ASSERT_EQ(test_sink_ptr_->count(), 1);
    EXPECT_TRUE(test_sink_ptr_->contains_message("Error"));
}

TEST_F(QuillLoggerTest, LogsAllLevelsWhenSetToDebug) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Debug);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Debug, "Debug");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info");
    PUBSUB_LOG_STR(logger, LogLevel::Warning, "Warning");
    PUBSUB_LOG_STR(logger, LogLevel::Error, "Error");
    PUBSUB_LOG_STR(logger, LogLevel::Critical, "Critical");

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
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act - first log at Info level
    PUBSUB_LOG_STR(logger, LogLevel::Debug, "Debug 1 - filtered");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info 1 - appears");

    // Change to Debug level
    logger.set_log_level(LogLevel::Debug);

    PUBSUB_LOG_STR(logger, LogLevel::Debug, "Debug 2 - appears");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Info 2 - appears");

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
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Message 1");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Message 2");
    PUBSUB_LOG_STR(logger, LogLevel::Info, "Message 3");

    // Assert
    EXPECT_EQ(test_sink_ptr_->count(), 3);
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
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Assert
    EXPECT_TRUE(logger.should_log_to_file(LogLevel::Error));
    EXPECT_TRUE(logger.should_log_to_file(LogLevel::Warning));
    EXPECT_TRUE(logger.should_log_to_file(LogLevel::Info));
    EXPECT_FALSE(logger.should_log_to_file(LogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToConsoleRespectsLevel) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Warning);

    // Assert
    EXPECT_TRUE(logger.should_log_to_console(LogLevel::Critical));
    EXPECT_TRUE(logger.should_log_to_console(LogLevel::Error));
    EXPECT_TRUE(logger.should_log_to_console(LogLevel::Warning));
    EXPECT_FALSE(logger.should_log_to_console(LogLevel::Info));
    EXPECT_FALSE(logger.should_log_to_console(LogLevel::Debug));
}

TEST_F(QuillLoggerTest, ShouldLogToSyslogRespectsLevel) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Error);

    // Assert
    EXPECT_TRUE(logger.should_log_to_syslog(LogLevel::Critical));
    EXPECT_TRUE(logger.should_log_to_syslog(LogLevel::Error));
    EXPECT_FALSE(logger.should_log_to_syslog(LogLevel::Warning));
    EXPECT_FALSE(logger.should_log_to_syslog(LogLevel::Info));
}

// =============================================================================
// Log Level Getter Test
// =============================================================================

TEST_F(QuillLoggerTest, ReturnsCorrectLogLevel) {
    // Arrange & Act
    QuillLogger logger(test_sink_, LogLevel::Warning);

    // Assert
    EXPECT_EQ(logger.log_level(), LogLevel::Warning);
}

TEST_F(QuillLoggerTest, ReturnsUpdatedLogLevelAfterChange) {
    // Arrange
    QuillLogger logger(test_sink_, LogLevel::Info);

    // Act
    logger.set_log_level(LogLevel::Error);

    // Assert
    EXPECT_EQ(logger.log_level(), LogLevel::Error);
}
