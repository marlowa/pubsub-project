// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/LoggingConfigurationLoader.hpp>
#include <pubsub_itc_fw/RollingLogfileConfiguration.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

using namespace pubsub_itc_fw;

namespace {

void load_toml(TomlConfiguration& config, const char* content) {
    const auto [ok, err] = config.load_string(content);
    ASSERT_TRUE(ok) << err;
}

} // namespaces

TEST(LoggingConfigurationLoaderTest, SizeModeReadsSizeParameters) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        mode = "size"
        max_file_size = 1048576
        max_backup_files = 5
    )");

    const RollingLogfileConfiguration result = LoggingConfigurationLoader::load(config);

    EXPECT_EQ(result.mode, RollingLogfileConfiguration::Mode::Size);
    EXPECT_EQ(result.max_file_size, 1048576);
    EXPECT_EQ(result.max_backup_files, 5);
}

TEST(LoggingConfigurationLoaderTest, DailyModeReadsRotationTime) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        mode = "daily"
        rotation_time = "03:30"
    )");

    const RollingLogfileConfiguration result = LoggingConfigurationLoader::load(config);

    EXPECT_EQ(result.mode, RollingLogfileConfiguration::Mode::Daily);
    EXPECT_EQ(result.rotation_time, "03:30");
}

TEST(LoggingConfigurationLoaderTest, NoneModeNeedsNoFurtherKeys) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        mode = "none"
    )");

    const RollingLogfileConfiguration result = LoggingConfigurationLoader::load(config);

    EXPECT_EQ(result.mode, RollingLogfileConfiguration::Mode::None);
}

TEST(LoggingConfigurationLoaderTest, UnknownModeThrows) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        mode = "hourly"
    )");

    EXPECT_THROW(LoggingConfigurationLoader::load(config), ConfigurationException);
}

TEST(LoggingConfigurationLoaderTest, MissingModeThrows) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        max_file_size = 1048576
    )");

    EXPECT_THROW(LoggingConfigurationLoader::load(config), ConfigurationException);
}

TEST(LoggingConfigurationLoaderTest, SizeModeMissingRequiredKeyThrows) {
    TomlConfiguration config;
    load_toml(config, R"(
        [logging]
        mode = "size"
        max_backup_files = 5
    )");

    EXPECT_THROW(LoggingConfigurationLoader::load(config), ConfigurationException);
}
