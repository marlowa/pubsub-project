#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <utility>  // for std::pair
#include <iterator> // for std::istreambuf_iterator

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/Logger.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>

/*
Some important notes about testing the logger:

The logger is based on quill and quill very much assumes that a single logger is used through the program.
This not only means that we need a single logger object in this test, it also means that when we log data
we have to be aware that will be maintaining an open file handle for that logfile.
This means we cannot delete the logfile between tests. We essentially need all our tests to be in one test.
If we had multiple tests, deleting logfiles between each test, then quill would hang on to the open file
handle and write to a file no longer visible to use. This means tests would work individually but not when
run one after the other.

Also note: We would like a test that employs code such as this:
    PUBSUB_LOG(test_logger, LogLevel::Info, "Bad format {} {} {}", "only", "two");
This is to see if we get a compilation error due to the mismatch.
However, there is no way at the moment to write such a test using gtest.
 */

namespace {
// NOLINTNEXTLINE(misc-include-cleaner)
boost::filesystem::path test_log_dir_{boost::filesystem::temp_directory_path() / "logger_test"};
pubsub_itc_fw::Logger test_logger(pubsub_itc_fw::LogLevel::Debug, test_log_dir_.string(), "test.log", pubsub_itc_fw::LoggerInterface::FilenameAppendMode::None,
                                  0);
} // namespace

namespace pubsub_itc_fw {

class LoggerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create temporary directory for test logs
        // NOLINTNEXTLINE(misc-include-cleaner)
        test_log_dir_ = boost::filesystem::temp_directory_path() / "logger_test";
        // NOLINTNEXTLINE(misc-include-cleaner)
        boost::filesystem::create_directories(test_log_dir_);
        test_logger.set_immediate_flush(); // provided for ease of debugging/testing.
    }

    void TearDown() override {
        // Clean up test log files
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (boost::filesystem::exists(test_log_dir_)) {
            // NOLINTNEXTLINE(misc-include-cleaner)
            boost::filesystem::remove_all(test_log_dir_);
        }
    }

    std::string readLogFile(const std::string& filename) {
        // Give quill a moment to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ifstream file(test_log_dir_ / filename);
        if (!file.is_open()) {
            return "";
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return content;
    }

    // Helper function to find log files that start with a given prefix
    std::pair<bool, std::string> findLogFileContent(const std::string& prefix) {
        if (!boost::filesystem::exists(test_log_dir_)) {
            return {false, ""};
        }

        // NOLINTNEXTLINE(misc-include-cleaner)
        for (const auto& entry : boost::filesystem::directory_iterator(test_log_dir_)) {
            const std::string filename = entry.path().filename().string();
            if (filename.find(prefix) == 0) // starts with prefix
            {
                std::ifstream file(entry.path());
                if (file.is_open()) {
                    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    return {true, content};
                }
            }
        }

        return {false, ""};
    }
};

TEST_F(LoggerTest, theOneTest) {
    // ShouldLogReturnsTrueForEqualOrHigherLevels
    test_logger.set_log_level(LogLevel::Info);

    EXPECT_TRUE(test_logger.should_log(LogLevel::Critical));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Error));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Warning));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Info));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Debug));

    // ShouldLogReturnsFalseForLowerLevels
    test_logger.set_log_level(LogLevel::Error);

    EXPECT_TRUE(test_logger.should_log(LogLevel::Critical));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Error));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Warning));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Info));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Debug));

    // BasicLoggingWritesToFile
    test_logger.set_log_level(LogLevel::Info);

    PUBSUB_LOG(test_logger, LogLevel::Info, "Test message: {}", "hello world");
    test_logger.flush();

    // Find and read the log file
    auto [found_log_file, log_content] = findLogFileContent("test.log");

    ASSERT_TRUE(found_log_file) << "Log file not found in directory: " << test_log_dir_;
    EXPECT_FALSE(log_content.empty());
    EXPECT_TRUE(log_content.find("Test message: hello world") != std::string::npos) << "Expected message not found in log content: [" << log_content << "]";
    EXPECT_TRUE(log_content.find("LOG_INFO") != std::string::npos) << "LOG_INFO not found in log content: [" << log_content << "]";

    // LoggingRespectsLogLevel
    test_logger.set_log_level(LogLevel::Warning);

    // This should be logged
    PUBSUB_LOG_STR(test_logger, LogLevel::Error, "Error message should be logged");

    // This should not be logged
    PUBSUB_LOG_STR(test_logger, LogLevel::Info, "Info message should not be logged");

    // Flush to ensure messages are written
    test_logger.flush();

    // Find and read the log file
    auto [found_log_file2, log_content2] = findLogFileContent("test.log");

    ASSERT_TRUE(found_log_file2) << "Log file not found in directory: " << test_log_dir_;
    EXPECT_TRUE(log_content2.find("Error message should be logged") != std::string::npos) << "Error message not found in log content: " << log_content;
    EXPECT_TRUE(log_content2.find("Info message should not be logged") == std::string::npos) << "Info message should not be in log content: " << log_content;

    test_logger.set_log_level(LogLevel::Debug);
}

} // namespace pubsub_itc_fw
