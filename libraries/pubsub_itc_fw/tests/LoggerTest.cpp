#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <iterator>

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <pubsub_itc_fw/LogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace {
boost::filesystem::path test_log_dir_{boost::filesystem::temp_directory_path() / "logger_test"};

pubsub_itc_fw::QuillLogger test_logger;

} // namespace

namespace pubsub_itc_fw {

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_log_dir_ = boost::filesystem::temp_directory_path() / "logger_test";
        boost::filesystem::create_directories(test_log_dir_);
        test_logger.set_immediate_flush();
    }

    void TearDown() override {
        if (boost::filesystem::exists(test_log_dir_)) {
            boost::filesystem::remove_all(test_log_dir_);
        }
    }

    std::pair<bool, std::string> findLogFileContent(const std::string& prefix) {
        if (!boost::filesystem::exists(test_log_dir_)) {
            return {false, ""};
        }

        for (const auto& entry : boost::filesystem::directory_iterator(test_log_dir_)) {
            const std::string filename = entry.path().filename().string();
            if (filename.find(prefix) == 0) {
                std::ifstream file(entry.path());
                if (file.is_open()) {
                    const std::string content(
                        (std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
                    return {true, content};
                }
            }
        }

        return {false, ""};
    }
};

TEST_F(LoggerTest, theOneTest) {
    //
    // 1. should_log semantics
    //
    test_logger.set_log_level(LogLevel::Info);

    EXPECT_TRUE(test_logger.should_log(LogLevel::Critical));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Error));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Warning));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Info));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Debug));

    test_logger.set_log_level(LogLevel::Error);

    EXPECT_TRUE(test_logger.should_log(LogLevel::Critical));
    EXPECT_TRUE(test_logger.should_log(LogLevel::Error));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Warning));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Info));
    EXPECT_FALSE(test_logger.should_log(LogLevel::Debug));

    //
    // 2. Basic logging + metadata + formatting
    //
    test_logger.set_log_level(LogLevel::Info);

    PUBSUB_LOG(test_logger, LogLevel::Info, "Test message: {}", "hello world");
    test_logger.flush();

    auto [found1, content1] = findLogFileContent("test.log");
    ASSERT_TRUE(found1);
    EXPECT_FALSE(content1.empty());

    // Metadata must appear
    EXPECT_NE(content1.find("LoggerTest.cpp"), std::string::npos);
    EXPECT_NE(content1.find("theOneTest"), std::string::npos);

    // Message content must appear (order not enforced)
    EXPECT_NE(content1.find("Test message:"), std::string::npos);
    EXPECT_NE(content1.find("hello world"), std::string::npos);

    //
    // 3. Multi‑argument formatting
    //
    PUBSUB_LOG(test_logger, LogLevel::Info, "Two values: {} {}", 1, 2);
    test_logger.flush();

    auto [found2, content2] = findLogFileContent("test.log");
    ASSERT_TRUE(found2);

    EXPECT_NE(content2.find("Two values:"), std::string::npos);
    EXPECT_NE(content2.find("1 2"), std::string::npos);

    //
    // 4. Multiple messages + ordering (weak check)
    //
    PUBSUB_LOG_STR(test_logger, LogLevel::Info, "First message");
    PUBSUB_LOG_STR(test_logger, LogLevel::Info, "Second message");
    test_logger.flush();

    auto [found3, content3] = findLogFileContent("test.log");
    ASSERT_TRUE(found3);

    auto pos_first = content3.find("First message");
    auto pos_second = content3.find("Second message");

    EXPECT_NE(pos_first, std::string::npos);
    EXPECT_NE(pos_second, std::string::npos);
    EXPECT_LT(pos_first, pos_second);

    //
    // 5. Log‑level filtering in actual output
    //
    test_logger.set_log_level(LogLevel::Warning);

    PUBSUB_LOG_STR(test_logger, LogLevel::Error, "Error message should be logged");
    PUBSUB_LOG_STR(test_logger, LogLevel::Info, "Info message should not be logged");

    test_logger.flush();

    auto [found4, content4] = findLogFileContent("test.log");
    ASSERT_TRUE(found4);

    EXPECT_NE(content4.find("Error message should be logged"), std::string::npos);
    EXPECT_EQ(content4.find("Info message should not be logged"), std::string::npos);

    //
    // 6. Immediate flush behavior
    //
    test_logger.set_log_level(LogLevel::Info);
    test_logger.set_immediate_flush();

    PUBSUB_LOG_STR(test_logger, LogLevel::Info, "Immediate flush test");

    auto [found5, content5] = findLogFileContent("test.log");
    ASSERT_TRUE(found5);

    EXPECT_NE(content5.find("Immediate flush test"), std::string::npos);

    // Reset
    test_logger.set_log_level(LogLevel::Debug);
}

} // namespace pubsub_itc_fw
