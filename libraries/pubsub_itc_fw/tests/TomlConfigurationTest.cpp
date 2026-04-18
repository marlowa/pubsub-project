// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @brief Unit tests for TomlConfiguration.
 *
 * Tests cover:
 *   - load_string: valid and invalid TOML
 *   - set / get_required for all supported types
 *   - Nested key access using dotted path notation
 *   - Duration suffix parsing and lossless/lossy conversion rules
 *   - get_required error paths: missing key, wrong type, out-of-range
 *   - get_required_except: throws ConfigurationException on error
 *   - Multiple fetches in a try block using get_required_except
 *   - load_file: missing file returns error
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw::tests {

class TomlConfigurationTest : public ::testing::Test {
protected:
    TomlConfiguration config;
};

// ============================================================
// load_string
// ============================================================

TEST_F(TomlConfigurationTest, LoadStringValidToml)
{
    auto [ok, err] = config.load_string(R"(
        [gateway]
        sender_comp_id = "GATEWAY"
        listen_port = 9878
    )");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(err.empty());
}

TEST_F(TomlConfigurationTest, LoadStringInvalidTomlReturnsFalse)
{
    auto [ok, err] = config.load_string("this is not valid toml ][[[");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

TEST_F(TomlConfigurationTest, LoadStringInvalidTomlIncludesLineNumber)
{
    auto [ok, err] = config.load_string("key = \n][[[");
    EXPECT_FALSE(ok);
    // Error message should contain a line number
    EXPECT_NE(err.find("line"), std::string::npos);
}

TEST_F(TomlConfigurationTest, LoadStringLeavesConfigUnchangedOnFailure)
{
    // Load valid config first
    auto [ok1, err1] = config.load_string(R"(name = "original")");
    EXPECT_TRUE(ok1);

    // Attempt to load invalid config -- should leave config unchanged
    auto [ok2, err2] = config.load_string("not valid ][");
    EXPECT_FALSE(ok2);

    // Original value should still be accessible
    std::string name;
    auto [ok3, err3] = config.get_required("name", name);
    EXPECT_TRUE(ok3);
    EXPECT_EQ(name, "original");
}

// ============================================================
// load_file
// ============================================================

TEST_F(TomlConfigurationTest, LoadFileNonExistentFileReturnsFalse)
{
    auto [ok, err] = config.load_file("/tmp/does_not_exist_12345.toml");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

// ============================================================
// set / get_required: std::string
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetString)
{
    config.set("name", std::string{"hello"});
    std::string value;
    auto [ok, err] = config.get_required("name", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, "hello");
}

TEST_F(TomlConfigurationTest, GetStringMissingKeyReturnsFalse)
{
    std::string value;
    auto [ok, err] = config.get_required("missing_key", value);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("missing_key"), std::string::npos);
}

TEST_F(TomlConfigurationTest, GetStringWrongTypeReturnsFalse)
{
    config.set("count", int32_t{42});
    std::string value;
    auto [ok, err] = config.get_required("count", value);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

// ============================================================
// set / get_required: bool
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetBoolTrue)
{
    config.set("flag", true);
    bool value = false;
    auto [ok, err] = config.get_required("flag", value);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(value);
}

TEST_F(TomlConfigurationTest, SetAndGetBoolFalse)
{
    config.set("flag", false);
    bool value = true;
    auto [ok, err] = config.get_required("flag", value);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(value);
}

TEST_F(TomlConfigurationTest, GetBoolWrongTypeReturnsFalse)
{
    config.set("name", std::string{"hello"});
    bool value = false;
    auto [ok, err] = config.get_required("name", value);
    EXPECT_FALSE(ok);
}

// ============================================================
// set / get_required: int32_t
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetInt32)
{
    config.set("port", int32_t{9878});
    int32_t value = 0;
    auto [ok, err] = config.get_required("port", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, 9878);
}

TEST_F(TomlConfigurationTest, GetInt32OutOfRangeReturnsFalse)
{
    // Set a value that is valid int64 but out of int32 range
    config.set("big", int64_t{3'000'000'000LL});
    int32_t value = 0;
    auto [ok, err] = config.get_required("big", value);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("range"), std::string::npos);
}

// ============================================================
// set / get_required: int64_t
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetInt64)
{
    config.set("big", int64_t{3'000'000'000LL});
    int64_t value = 0;
    auto [ok, err] = config.get_required("big", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, 3'000'000'000LL);
}

// ============================================================
// set / get_required: double
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetDouble)
{
    config.set("ratio", 3.14);
    double value = 0.0;
    auto [ok, err] = config.get_required("ratio", value);
    EXPECT_TRUE(ok);
    EXPECT_DOUBLE_EQ(value, 3.14);
}

// ============================================================
// Nested key access
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetNestedKey)
{
    config.set("gateway.sender_comp_id", std::string{"GATEWAY"});
    config.set("gateway.listen_port", int32_t{9878});

    std::string sender;
    int32_t port = 0;
    auto [ok1, err1] = config.get_required("gateway.sender_comp_id", sender);
    auto [ok2, err2] = config.get_required("gateway.listen_port", port);

    EXPECT_TRUE(ok1);
    EXPECT_EQ(sender, "GATEWAY");
    EXPECT_TRUE(ok2);
    EXPECT_EQ(port, 9878);
}

TEST_F(TomlConfigurationTest, LoadStringNestedKey)
{
    auto [ok, err] = config.load_string(R"(
        [gateway]
        sender_comp_id = "GATEWAY"
        listen_port = 9878
    )");
    EXPECT_TRUE(ok);

    std::string sender;
    int32_t port = 0;
    auto [ok1, err1] = config.get_required("gateway.sender_comp_id", sender);
    auto [ok2, err2] = config.get_required("gateway.listen_port", port);

    EXPECT_TRUE(ok1);
    EXPECT_EQ(sender, "GATEWAY");
    EXPECT_TRUE(ok2);
    EXPECT_EQ(port, 9878);
}

// ============================================================
// Duration: set and get same type
// ============================================================

TEST_F(TomlConfigurationTest, SetAndGetSeconds)
{
    config.set("timeout", std::chrono::seconds{30});
    std::chrono::seconds value{0};
    auto [ok, err] = config.get_required("timeout", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::seconds{30});
}

TEST_F(TomlConfigurationTest, SetAndGetMilliseconds)
{
    config.set("interval", std::chrono::milliseconds{500});
    std::chrono::milliseconds value{0};
    auto [ok, err] = config.get_required("interval", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::milliseconds{500});
}

TEST_F(TomlConfigurationTest, SetAndGetMicroseconds)
{
    config.set("latency", std::chrono::microseconds{100});
    std::chrono::microseconds value{0};
    auto [ok, err] = config.get_required("latency", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::microseconds{100});
}

TEST_F(TomlConfigurationTest, SetAndGetNanoseconds)
{
    config.set("precision", std::chrono::nanoseconds{500});
    std::chrono::nanoseconds value{0};
    auto [ok, err] = config.get_required("precision", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::nanoseconds{500});
}

TEST_F(TomlConfigurationTest, SetAndGetMinutes)
{
    config.set("window", std::chrono::minutes{5});
    std::chrono::minutes value{0};
    auto [ok, err] = config.get_required("window", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::minutes{5});
}

TEST_F(TomlConfigurationTest, SetAndGetHours)
{
    config.set("session", std::chrono::hours{8});
    std::chrono::hours value{0};
    auto [ok, err] = config.get_required("session", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::hours{8});
}

// ============================================================
// Duration: lossless conversion (coarse to fine)
// ============================================================

TEST_F(TomlConfigurationTest, SecondsConvertedToMilliseconds)
{
    config.set("timeout", std::chrono::seconds{30});
    std::chrono::milliseconds value{0};
    auto [ok, err] = config.get_required("timeout", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::milliseconds{30'000});
}

TEST_F(TomlConfigurationTest, SecondsConvertedToMicroseconds)
{
    config.set("timeout", std::chrono::seconds{1});
    std::chrono::microseconds value{0};
    auto [ok, err] = config.get_required("timeout", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::microseconds{1'000'000});
}

TEST_F(TomlConfigurationTest, MillisecondsConvertedToMicroseconds)
{
    config.set("interval", std::chrono::milliseconds{5});
    std::chrono::microseconds value{0};
    auto [ok, err] = config.get_required("interval", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::microseconds{5'000});
}

// ============================================================
// Duration: lossless conversion (fine to coarse, exact)
// ============================================================

TEST_F(TomlConfigurationTest, ExactMillisecondsConvertedToSeconds)
{
    config.set("timeout", std::chrono::milliseconds{1000});
    std::chrono::seconds value{0};
    auto [ok, err] = config.get_required("timeout", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::seconds{1});
}

TEST_F(TomlConfigurationTest, ExactMicrosecondsConvertedToMilliseconds)
{
    config.set("latency", std::chrono::microseconds{2000});
    std::chrono::milliseconds value{0};
    auto [ok, err] = config.get_required("latency", value);
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, std::chrono::milliseconds{2});
}

// ============================================================
// Duration: lossy conversion (fine to coarse, not exact)
// ============================================================

TEST_F(TomlConfigurationTest, LossyMillisecondsToSecondsReturnsFalse)
{
    config.set("timeout", std::chrono::milliseconds{500});
    std::chrono::seconds value{0};
    auto [ok, err] = config.get_required("timeout", value);
    EXPECT_FALSE(ok);
    EXPECT_NE(err.find("precision"), std::string::npos);
}

TEST_F(TomlConfigurationTest, LossyMicrosecondsToMillisecondsReturnsFalse)
{
    config.set("latency", std::chrono::microseconds{100});
    std::chrono::milliseconds value{0};
    auto [ok, err] = config.get_required("latency", value);
    EXPECT_FALSE(ok);
}

// ============================================================
// Duration: load_string with suffix
// ============================================================

TEST_F(TomlConfigurationTest, LoadStringDurationSeconds)
{
    auto [ok, err] = config.load_string(R"(logon_timeout = "30s")");
    EXPECT_TRUE(ok);
    std::chrono::seconds value{0};
    auto [ok2, err2] = config.get_required("logon_timeout", value);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(value, std::chrono::seconds{30});
}

TEST_F(TomlConfigurationTest, LoadStringDurationMilliseconds)
{
    auto [ok, err] = config.load_string(R"(interval = "500ms")");
    EXPECT_TRUE(ok);
    std::chrono::milliseconds value{0};
    auto [ok2, err2] = config.get_required("interval", value);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(value, std::chrono::milliseconds{500});
}

TEST_F(TomlConfigurationTest, LoadStringDurationMinutes)
{
    auto [ok, err] = config.load_string(R"(window = "5m")");
    EXPECT_TRUE(ok);
    std::chrono::minutes value{0};
    auto [ok2, err2] = config.get_required("window", value);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(value, std::chrono::minutes{5});
}

TEST_F(TomlConfigurationTest, LoadStringDurationHours)
{
    auto [ok, err] = config.load_string(R"(session = "8h")");
    EXPECT_TRUE(ok);
    std::chrono::hours value{0};
    auto [ok2, err2] = config.get_required("session", value);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(value, std::chrono::hours{8});
}

TEST_F(TomlConfigurationTest, LoadStringDurationUnknownSuffixReturnsFalse)
{
    auto [ok, err] = config.load_string(R"(timeout = "30x")");
    EXPECT_TRUE(ok); // parses as valid TOML string
    std::chrono::seconds value{0};
    auto [ok2, err2] = config.get_required("timeout", value);
    EXPECT_FALSE(ok2);
    EXPECT_NE(err2.find("unknown"), std::string::npos);
}

TEST_F(TomlConfigurationTest, LoadStringDurationMalformedReturnsFalse)
{
    auto [ok, err] = config.load_string(R"(timeout = "abcs")");
    EXPECT_TRUE(ok); // parses as valid TOML string
    std::chrono::seconds value{0};
    auto [ok2, err2] = config.get_required("timeout", value);
    EXPECT_FALSE(ok2);
}

// ============================================================
// get_required_except: throws ConfigurationException
// ============================================================

TEST_F(TomlConfigurationTest, GetRequiredExceptThrowsOnMissingKey)
{
    std::string value;
    EXPECT_THROW(
        config.get_required_except("missing_key", value),
        ConfigurationException);
}

TEST_F(TomlConfigurationTest, GetRequiredExceptThrowsOnWrongType)
{
    config.set("count", int32_t{42});
    std::string value;
    EXPECT_THROW(
        config.get_required_except("count", value),
        ConfigurationException);
}

TEST_F(TomlConfigurationTest, GetRequiredExceptThrowsOnLossyDurationConversion)
{
    config.set("timeout", std::chrono::milliseconds{500});
    std::chrono::seconds value{0};
    EXPECT_THROW(
        config.get_required_except("timeout", value),
        ConfigurationException);
}

TEST_F(TomlConfigurationTest, GetRequiredExceptSucceedsWhenKeyPresent)
{
    config.set("name", std::string{"GATEWAY"});
    std::string value;
    EXPECT_NO_THROW(config.get_required_except("name", value));
    EXPECT_EQ(value, "GATEWAY");
}

TEST_F(TomlConfigurationTest, GetRequiredExceptMultipleFetchesInTryBlock)
{
    config.set("gateway.sender_comp_id", std::string{"GATEWAY"});
    config.set("gateway.listen_port",    int32_t{9878});
    config.set("gateway.logon_timeout",  std::chrono::seconds{30});

    std::string sender;
    int32_t     port{0};
    std::chrono::seconds timeout{0};

    EXPECT_NO_THROW({
        try {
            config.get_required_except("gateway.sender_comp_id", sender);
            config.get_required_except("gateway.listen_port",    port);
            config.get_required_except("gateway.logon_timeout",  timeout);
        } catch (const ConfigurationException& ex) {
            FAIL() << "Unexpected ConfigurationException: " << ex.what();
        }
    });

    EXPECT_EQ(sender,  "GATEWAY");
    EXPECT_EQ(port,    9878);
    EXPECT_EQ(timeout, std::chrono::seconds{30});
}

TEST_F(TomlConfigurationTest, GetRequiredExceptFirstFailureAbortsTryBlock)
{
    config.set("gateway.sender_comp_id", std::string{"GATEWAY"});
    // listen_port intentionally missing

    std::string sender;
    int32_t     port{0};

    bool exception_thrown = false;
    try {
        config.get_required_except("gateway.sender_comp_id", sender);
        config.get_required_except("gateway.listen_port",    port);
    } catch (const ConfigurationException&) {
        exception_thrown = true;
    }

    EXPECT_TRUE(exception_thrown);
    EXPECT_EQ(sender, "GATEWAY"); // first fetch succeeded
    EXPECT_EQ(port,   0);         // second fetch never completed
}

} // namespace pubsub_itc_fw::tests
