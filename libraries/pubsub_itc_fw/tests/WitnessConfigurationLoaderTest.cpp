// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>
#include <pubsub_itc_fw/WitnessConfiguration.hpp>
#include <pubsub_itc_fw/WitnessConfigurationLoader.hpp>

namespace {

// Everything the loader requires apart from the pair, taken from applications/witness/witness.toml
// with values in place of the deployment's placeholders, and with pinning off, so that the metrics
// section is not needed.
constexpr const char* required_sections = R"(
[network]
listen_host = "127.0.0.1"
listen_port = 11100

[ha]
enabled = true

[lease]
period_milliseconds = 3000
drift_allowance_milliseconds = 250
renewal_interval_milliseconds = 1000

[logging]
applog_level = "info"
syslog_level = "critical"

[reactor]
cpu_pinning_enabled = false
cpu_pinning_reserve_cpu0 = true
connect_retry_warning_interval = "15m"

[event_queue_pool]
objects_per_slab = 64
initial_slabs    = 1

[command_queue_pool]
objects_per_slab = 64
initial_slabs    = 1
)";

pubsub_itc_fw::WitnessConfiguration load_with_pair(const std::string& pair_table) {
    pubsub_itc_fw::TomlConfiguration toml;
    const auto [ok, err] = toml.load_string(std::string(required_sections) + pair_table);
    EXPECT_TRUE(ok) << err;
    return pubsub_itc_fw::WitnessConfigurationLoader::load(toml);
}

// The message of the ConfigurationException that loading @p pair_table throws, or empty if it does not throw.
std::string refusal_for(const std::string& pair_table) {
    try {
        static_cast<void>(load_with_pair(pair_table));
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        return ex.what();
    }
    return {};
}

} // namespaces

TEST(WitnessConfigurationLoaderTest, ReadsThePairItVotesIn) {
    const pubsub_itc_fw::WitnessConfiguration config = load_with_pair(R"(
[voting_pair]
name    = "arbiter"
pair_id = 4
)");
    EXPECT_EQ(config.voting_pair.name, "arbiter");
    EXPECT_EQ(config.voting_pair.pair_id, 4);
}

TEST(WitnessConfigurationLoaderTest, RefusesAMissingPair) {
    EXPECT_NE(refusal_for("").find("voting_pair"), std::string::npos);
}

TEST(WitnessConfigurationLoaderTest, RefusesAPairNumberThatIsNotPositive) {
    const std::string refusal = refusal_for(R"(
[voting_pair]
name    = "arbiter"
pair_id = -1
)");
    EXPECT_NE(refusal.find("voting_pair.pair_id must be positive"), std::string::npos) << refusal;
}

TEST(WitnessConfigurationLoaderTest, RefusesAnEmptyName) {
    const std::string refusal = refusal_for(R"(
[voting_pair]
name    = ""
pair_id = 4
)");
    EXPECT_NE(refusal.find("voting_pair.name must not be empty"), std::string::npos) << refusal;
}
