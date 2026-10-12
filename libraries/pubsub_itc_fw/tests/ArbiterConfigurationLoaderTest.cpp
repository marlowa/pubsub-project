// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ArbiterConfiguration.hpp>
#include <pubsub_itc_fw/ArbiterConfigurationLoader.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace {

// Everything the loader requires apart from the pairs, so that each test below varies only the
// [[voting_pair]] and [own_pair] tables and nothing else can be why it fails. Taken from
// applications/arbiter/arbiter_primary.toml with values in place of the deployment's placeholders,
// and with pinning off, so that the metrics section is not needed.
constexpr const char* required_sections = R"(
[network]
listen_host = "127.0.0.1"
listen_port = 11200

[ha]
enabled     = true
instance_id = 1

[peer]
instance_id = 2
listen_host = "127.0.0.1"
listen_port = 11203
host        = "127.0.0.1"
port        = 11204

[witness]
host = "127.0.0.1"
port = 11100

[lease]
period_milliseconds = 3000
drift_allowance_milliseconds = 250
renewal_interval_milliseconds = 1000
promise_file = "var/arbiter_primary.promise"

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

// The venue's pairs, as arbiter_primary.toml configures them.
constexpr const char* venue_pairs = R"(
[own_pair]
name    = "arbiter"
pair_id = 4

[[voting_pair]]
name    = "sequencer"
pair_id = 1

[[voting_pair]]
name    = "matching_engine"
pair_id = 2

[[voting_pair]]
name    = "matching_engine_publisher"
pair_id = 3
)";

pubsub_itc_fw::ArbiterConfiguration load_with_pairs(const std::string& pair_tables) {
    pubsub_itc_fw::TomlConfiguration toml;
    const auto [ok, err] = toml.load_string(std::string(required_sections) + pair_tables);
    EXPECT_TRUE(ok) << err;
    return pubsub_itc_fw::ArbiterConfigurationLoader::load(toml);
}

// The message of the ConfigurationException that loading @p pair_tables throws, or empty if it does not throw.
std::string refusal_for(const std::string& pair_tables) {
    try {
        static_cast<void>(load_with_pairs(pair_tables));
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        return ex.what();
    }
    return {};
}

} // namespaces

TEST(ArbiterConfigurationLoaderTest, ReadsEveryPairInOrderAndTheArbitersOwnPair) {
    const pubsub_itc_fw::ArbiterConfiguration config = load_with_pairs(venue_pairs);

    ASSERT_EQ(config.voting_pairs.size(), 3U);
    EXPECT_EQ(config.voting_pairs[0].name, "sequencer");
    EXPECT_EQ(config.voting_pairs[0].pair_id, 1);
    EXPECT_EQ(config.voting_pairs[1].name, "matching_engine");
    EXPECT_EQ(config.voting_pairs[1].pair_id, 2);
    EXPECT_EQ(config.voting_pairs[2].name, "matching_engine_publisher");
    EXPECT_EQ(config.voting_pairs[2].pair_id, 3);
    EXPECT_EQ(config.own_pair.name, "arbiter");
    EXPECT_EQ(config.own_pair.pair_id, 4);
}

TEST(ArbiterConfigurationLoaderTest, RefusesNoPairsToVoteIn) {
    const std::string refusal = refusal_for(R"(
[own_pair]
name    = "arbiter"
pair_id = 4
)");
    EXPECT_NE(refusal.find("no [[voting_pair]]"), std::string::npos) << refusal;
}

TEST(ArbiterConfigurationLoaderTest, RefusesANumberGivenToTwoPairs) {
    const std::string refusal = refusal_for(R"(
[own_pair]
name    = "arbiter"
pair_id = 4

[[voting_pair]]
name    = "sequencer"
pair_id = 1

[[voting_pair]]
name    = "matching_engine"
pair_id = 1
)");
    EXPECT_NE(refusal.find("pair_id 1 is given to both"), std::string::npos) << refusal;
}

TEST(ArbiterConfigurationLoaderTest, RefusesANameGivenToTwoPairs) {
    const std::string refusal = refusal_for(R"(
[own_pair]
name    = "arbiter"
pair_id = 4

[[voting_pair]]
name    = "sequencer"
pair_id = 1

[[voting_pair]]
name    = "sequencer"
pair_id = 2
)");
    EXPECT_NE(refusal.find("the name 'sequencer' is given to two pairs"), std::string::npos) << refusal;
}

TEST(ArbiterConfigurationLoaderTest, RefusesTheArbitersOwnPairAmongThePairsItVotesIn) {
    const std::string refusal = refusal_for(R"(
[own_pair]
name    = "arbiter"
pair_id = 4

[[voting_pair]]
name    = "sequencer"
pair_id = 1

[[voting_pair]]
name    = "arbiter"
pair_id = 4
)");
    EXPECT_NE(refusal.find("is also listed in [[voting_pair]]"), std::string::npos) << refusal;
}

TEST(ArbiterConfigurationLoaderTest, RefusesAPairNumberThatIsNotPositive) {
    const std::string refusal = refusal_for(R"(
[own_pair]
name    = "arbiter"
pair_id = 4

[[voting_pair]]
name    = "sequencer"
pair_id = 0
)");
    EXPECT_NE(refusal.find("voting_pair[0].pair_id must be positive"), std::string::npos) << refusal;
}

TEST(ArbiterConfigurationLoaderTest, RefusesAMissingOwnPair) {
    const std::string refusal = refusal_for(R"(
[[voting_pair]]
name    = "sequencer"
pair_id = 1
)");
    EXPECT_NE(refusal.find("own_pair"), std::string::npos) << refusal;
}
