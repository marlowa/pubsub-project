/**
 * @file StringUtilsTest.cpp
 * @brief Unit tests for the StringUtils class.
 *
 * This file contains a suite of Google Tests to verify the correctness and
 * robustness of the `StringUtils` utility functions.
 */

// C++ headers whose names start with ‘c’
#include <cerrno>

// System C++ headers
#include <string>
#include <string_view>

// Third party headers
#include <gtest/gtest.h>

// Project headers
#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw {

// Define a test fixture for StringUtils
class StringUtilsTest : public ::testing::Test {};

// Test the various `starts_with` overloads
TEST_F(StringUtilsTest, StartsWith) {
    // Case 1: string and string_view overloads
    EXPECT_TRUE(StringUtils::starts_with("hello world", "hello"));
    EXPECT_TRUE(StringUtils::starts_with("hello world", std::string_view("hello")));

    // Case 2: C-style string overload
    EXPECT_TRUE(StringUtils::starts_with("hello world", "hello"));

    // Case 3: Negative cases
    EXPECT_FALSE(StringUtils::starts_with("hello world", "world"));
    EXPECT_FALSE(StringUtils::starts_with("hello world", "Hello")); // Case-sensitive

    // Case 4: Longer prefix than string
    EXPECT_FALSE(StringUtils::starts_with("hello", "hello world"));

    // Case 5: Empty prefix
    EXPECT_TRUE(StringUtils::starts_with("hello world", ""));
    EXPECT_TRUE(StringUtils::starts_with("hello world", std::string_view("")));
    EXPECT_TRUE(StringUtils::starts_with("hello world", ""));

    // Case 6: Empty string
    EXPECT_FALSE(StringUtils::starts_with("", "hello"));
    EXPECT_TRUE(StringUtils::starts_with("", ""));
}

// Test the `get_error_string` function
TEST_F(StringUtilsTest, GetErrorString) {
    // Test with a known error number (EAGAIN / EWOULDBLOCK)
    const std::string error_message = StringUtils::get_error_string(EAGAIN);
    EXPECT_FALSE(error_message.empty());
    // The exact message can vary by OS, but it should not be a generic fallback.
    EXPECT_NE(error_message, "Failed to get strerror_r message for error " + std::to_string(EAGAIN) + ". strerror_r returned nullptr.");

    // Test with a different known error number
    const std::string permission_denied_message = StringUtils::get_error_string(EACCES);
    EXPECT_FALSE(permission_denied_message.empty());

    // Test with a bogus error number to trigger the fallback path
    const std::string bogus_error_message = StringUtils::get_error_string(99999);
    EXPECT_FALSE(bogus_error_message.empty());
}

// Test the `leafname` function
TEST_F(StringUtilsTest, Leafname) {
    // Case 1: Path with forward slashes (Linux/macOS style)
    EXPECT_EQ(StringUtils::leafname("/usr/local/bin/my_app"), "my_app");
    EXPECT_EQ(StringUtils::leafname("path/to/file.txt"), "file.txt");

    // Case 2: Path with backslashes (Windows style)
    EXPECT_EQ(StringUtils::leafname("C:\\Users\\John\\file.txt"), "file.txt");

    // Case 3: Path with mixed separators (not standard but should work)
    EXPECT_EQ(StringUtils::leafname("C:\\path/to\\file.txt"), "file.txt");

    // Case 4: No path separator, just a filename
    EXPECT_EQ(StringUtils::leafname("main.cpp"), "main.cpp");

    // Case 5: Directory name ending with a separator
    EXPECT_EQ(StringUtils::leafname("/path/to/dir/"), "");
    EXPECT_EQ(StringUtils::leafname("C:\\path\\to\\dir\\"), "");

    // Case 6: Empty string
    EXPECT_EQ(StringUtils::leafname(""), "");
}

} // namespace pubsub_itc_fw
