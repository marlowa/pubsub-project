// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @brief Unit tests for the StringUtils class.
 *
 * This file contains a suite of Google Tests to verify the correctness and
 * robustness of the `StringUtils` utility functions.
 */

#include <cerrno>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/StringUtils.hpp>

namespace pubsub_itc_fw::tests {

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

TEST_F(StringUtilsTest, StartsWithAllOverloadsExplicit) {
    // std::string overload
    EXPECT_TRUE(StringUtils::starts_with(std::string("hello world"), std::string("hello")));
    EXPECT_FALSE(StringUtils::starts_with(std::string("hello world"), std::string("world")));

    // std::string_view overload
    const std::string_view sv = "hello";
    EXPECT_TRUE(StringUtils::starts_with("hello world", sv));
    EXPECT_FALSE(StringUtils::starts_with("hello world", std::string_view("world")));

    // const char* overload — must use a variable to force this overload
    const char* cprefix = "hello";
    EXPECT_TRUE(StringUtils::starts_with("hello world", cprefix));

    const char* cprefix_bad = "world";
    EXPECT_FALSE(StringUtils::starts_with("hello world", cprefix_bad));

    // Edge cases
    EXPECT_TRUE(StringUtils::starts_with("hello world", "")); // empty prefix
    EXPECT_TRUE(StringUtils::starts_with("hello world", std::string_view("")));

    EXPECT_FALSE(StringUtils::starts_with("", "hello")); // empty string
    EXPECT_TRUE(StringUtils::starts_with("", ""));       // both empty
}

// Test the `hex_dump` function
TEST_F(StringUtilsTest, HexDumpEmpty) {
    const std::string out = StringUtils::hex_dump(nullptr, 0);
    EXPECT_EQ(out, "0 bytes\n");
}

TEST_F(StringUtilsTest, HexDumpSingleByte) {
    const uint8_t data[] = {0x41}; // 'A'

    const std::string out = StringUtils::hex_dump(data, 1);

    EXPECT_EQ(out, "1 bytes\n"
                   "0000: 41                                               |A               |\n");
}

TEST_F(StringUtilsTest, HexDumpExactLine) {
    uint8_t data[16];
    for (int i = 0; i < 16; ++i) {
        data[i] = static_cast<uint8_t>(32 + i); // printable ASCII
    }

    const std::string out = StringUtils::hex_dump(data, 16);

    EXPECT_EQ(out, "16 bytes\n"
                   "0000: 20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E 2F  | !\"#$%&'()*+,-./|\n");
}

TEST_F(StringUtilsTest, HexDumpMultiLine) {
    uint8_t data[20];
    for (int i = 0; i < 20; ++i) {
        data[i] = static_cast<uint8_t>(i);
    }

    const std::string out = StringUtils::hex_dump(data, 20);

    EXPECT_EQ(out, "20 bytes\n"
                   "0000: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  |................|\n"
                   "0010: 10 11 12 13                                      |....            |\n");
}

TEST_F(StringUtilsTest, HexDumpNonPrintableBecomesDot) {
    const uint8_t data[] = {0x00, 0x01, 0x1F, 0x7F, 0x80, 0xFF};

    const std::string out = StringUtils::hex_dump(data, sizeof(data));

    // Expect all ASCII positions to be dots
    EXPECT_NE(out.find("|......"), std::string::npos);
}

TEST_F(StringUtilsTest, HexDumpPrintableBoundaries) {
    const uint8_t data[] = {31, 32, 126, 127};

    const std::string out = StringUtils::hex_dump(data, sizeof(data));

    // 31 -> '.', 32 -> ' ', 126 -> '~', 127 -> '.'
    EXPECT_NE(out.find("|. ~."), std::string::npos);
}

TEST_F(StringUtilsTest, HexDumpNoControlCharacters) {
    uint8_t data[256];
    for (int i = 0; i < 256; ++i) {
        data[i] = static_cast<uint8_t>(i);
    }

    const std::string out = StringUtils::hex_dump(data, sizeof(data));

    for (char c : out) {
        if (c == '\n')
            continue;

        EXPECT_GE(static_cast<unsigned char>(c), 32) << "Found control character: " << static_cast<int>(c);

        EXPECT_LE(static_cast<unsigned char>(c), 126) << "Found non-ASCII printable: " << static_cast<int>(c);
    }
}

TEST_F(StringUtilsTest, HexDumpAlignment) {
    uint8_t data[32] = {0};

    const std::string out = StringUtils::hex_dump(data, sizeof(data));

    std::istringstream iss(out);
    std::string line;

    std::getline(iss, line); // header

    while (std::getline(iss, line)) {
        const auto pos = line.find(" |");
        ASSERT_NE(pos, std::string::npos);

        // ASCII column should be: " |" + 16 chars + "|"
        EXPECT_EQ(line.size() - pos, 19);
    }
}

TEST_F(StringUtilsTest, HexDumpLargeBufferSanity) {
    std::vector<uint8_t> data(1024, 0xAA);

    const std::string out = StringUtils::hex_dump(data.data(), data.size());

    EXPECT_TRUE(StringUtils::starts_with(out, "1024 bytes\n"));
    EXPECT_GT(out.size(), 1024u);
}

} // namespace pubsub_itc_fw::tests
