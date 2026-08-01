// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <set>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "SequencerConfigurationLoader.hpp"

namespace {

// Everything the loader requires apart from the gateway entries, so each test below
// varies only the [[gateway]] tables and nothing else can be the reason it fails.
// Everything the loader requires apart from the gateway tables, so each test varies only
// those and nothing else can be why it fails. Derived from the real
// applications/sequencer/sequencer_primary.toml with the dev environment values
// substituted, rather than hand-assembled: the required set is large, spread across the
// sequencer's own loader and the shared reactor one, and a hand-written list drifts.
constexpr const char* required_sections = R"(
[network]
listen_host    = "127.0.0.1"
listen_port    = 11001
er_listen_host = "127.0.0.1"
er_listen_port = 11021

[matching_engine]
host = "127.0.0.1"
port = 11020

[matching_engine_secondary]
host = "127.0.0.1"
port = 11023

[ha]
ha_enabled                  = true
instance_id                 = 1
arbiter_primary_host        = "127.0.0.1"
arbiter_primary_port        = 11200
arbiter_secondary_host      = "127.0.0.1"
arbiter_secondary_port      = 11201
arbitration_timeout_seconds = 3

[peer]
listen_host                      = "127.0.0.1"
listen_port                      = 11003
host                             = "127.0.0.1"
port                             = 11004
heartbeat_interval_seconds       = 2
heartbeat_timeout_seconds        = 6
startup_election_timeout_seconds = 20

[wal_subscriber]
listen_host = "127.0.0.1"
listen_port = 11030

[wal]
directory                 = "var/sequencer_wal"
segment_size              = 4194304
snapshot_interval_seconds = 30

[logging]
applog_level = "info"
syslog_level = "critical"

[reactor]
cpu_pinning_enabled    = true
cpu_pinning_reserve_cpu0   = true
cpu_registry_shm_path  = "/pubsub_cpu_registry_test"
cpu_registry_lock_file = "/tmp/pubsub_cpu_registry_test.lock"
cpu_layout_file = "/tmp/cpu_layout_test.toml"
cpu_layout_component = "sequencer_primary"
connect_retry_warning_interval = "15m"

[event_queue_pool]
objects_per_slab = 81920
initial_slabs    = 1

[command_queue_pool]
objects_per_slab = 1500000
initial_slabs    = 1
)";

sequencer::SequencerConfiguration load_with_gateways(const std::string& gateway_tables) {
    pubsub_itc_fw::TomlConfiguration toml;
    const auto [ok, err] = toml.load_string(std::string(required_sections) + gateway_tables);
    EXPECT_TRUE(ok) << err;
    return sequencer::SequencerConfigurationLoader::load(toml);
}

} // namespaces

TEST(SequencerConfigurationLoaderTest, ReadsEveryGatewayEntry) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 2
instance = 1
host     = "10.0.0.2"
port     = 7110
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 2u);
    EXPECT_EQ(config.gateway_endpoints[0].protocol, 1);
    EXPECT_EQ(config.gateway_endpoints[0].instance, 1);
    EXPECT_EQ(config.gateway_endpoints[0].host, "10.0.0.1");
    EXPECT_EQ(config.gateway_endpoints[0].port, 7010);
    EXPECT_EQ(config.gateway_endpoints[1].protocol, 2);
    EXPECT_EQ(config.gateway_endpoints[1].port, 7110);
}

// The whole point of the collection: a protocol may now run as several processes.
TEST(SequencerConfigurationLoaderTest, ReadsTwoInstancesOfOneProtocol) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 1
instance = 2
host     = "10.0.0.2"
port     = 7011
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 2u);
    EXPECT_EQ(config.gateway_endpoints[0].instance, 1);
    EXPECT_EQ(config.gateway_endpoints[1].instance, 2);
    EXPECT_NE(config.gateway_endpoints[0].service_name(), config.gateway_endpoints[1].service_name());
}

// The shape dev actually deploys: two instances of each of the two protocols. Worth
// pinning down as one case rather than trusting that two passing pairs compose, because
// the failure this guards against -- two entries colliding on one service name -- only
// appears once both axes vary at the same time.
TEST(SequencerConfigurationLoaderTest, ReadsTwoInstancesOfEachOfTwoProtocols) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 1
instance = 2
host     = "10.0.0.2"
port     = 7011

[[gateway]]
protocol = 2
instance = 1
host     = "10.0.0.3"
port     = 7110

[[gateway]]
protocol = 2
instance = 2
host     = "10.0.0.4"
port     = 7111
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 4u);

    std::set<std::string> service_names;
    for (const auto& endpoint : config.gateway_endpoints) {
        service_names.insert(endpoint.service_name());
    }
    EXPECT_EQ(service_names.size(), 4u) << "every (protocol, instance) pair needs its own service name";

    EXPECT_EQ(config.gateway_endpoints[3].protocol, 2);
    EXPECT_EQ(config.gateway_endpoints[3].instance, 2);
    EXPECT_EQ(config.gateway_endpoints[3].port, 7111);
}

// Instance 1 of each protocol are different processes. If the service name collapsed the
// two axes, both would register under one name and one would silently win.
TEST(SequencerConfigurationLoaderTest, ServiceNamesSeparateTheTwoAxes) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 2
instance = 1
host     = "10.0.0.2"
port     = 7110
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 2u);
    EXPECT_NE(config.gateway_endpoints[0].service_name(), config.gateway_endpoints[1].service_name());
}

// Two entries on the same key would register one service name twice, and every report for
// that pair would go to whichever won -- silently, and differently depending on order.
TEST(SequencerConfigurationLoaderTest, RejectsDuplicateProtocolAndInstance) {
    EXPECT_THROW(load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.9"
port     = 7019
)"),
                 pubsub_itc_fw::ConfigurationException);
}

// enabled = false is how preprod, prod and test-1 run without the binary gateway. The
// entry stays in the file; the sequencer simply never dials it.
TEST(SequencerConfigurationLoaderTest, SkipsDisabledEntries) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
enabled  = true
host     = "10.0.0.1"
port     = 7010

[[gateway]]
protocol = 2
instance = 1
enabled  = false
host     = "10.0.0.2"
port     = 7110
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 1u);
    EXPECT_EQ(config.gateway_endpoints[0].protocol, 1);
}

TEST(SequencerConfigurationLoaderTest, AbsentEnabledMeansDeployed) {
    const auto config = load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 7010
)");

    ASSERT_EQ(config.gateway_endpoints.size(), 1u);
}

// Checked after filtering rather than on the raw table count. A config whose every gateway
// is disabled must fail here, not start a sequencer with nowhere to deliver reports.
TEST(SequencerConfigurationLoaderTest, RejectsAConfigWhereEveryGatewayIsDisabled) {
    EXPECT_THROW(load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
enabled  = false
host     = "10.0.0.1"
port     = 7010
)"),
                 pubsub_itc_fw::ConfigurationException);
}

TEST(SequencerConfigurationLoaderTest, RejectsNoGatewaysAtAll) {
    EXPECT_THROW(load_with_gateways(""), pubsub_itc_fw::ConfigurationException);
}

// Instances are numbered from 1, so 0 is not a lower bound to be defaulted -- it is a
// mistake, and one that would key a connection map entry nothing ever looks up.
TEST(SequencerConfigurationLoaderTest, RejectsInstanceBelowOne) {
    EXPECT_THROW(load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 0
host     = "10.0.0.1"
port     = 7010
)"),
                 pubsub_itc_fw::ConfigurationException);
}

TEST(SequencerConfigurationLoaderTest, RejectsAPortOutOfRange) {
    EXPECT_THROW(load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
port     = 70000
)"),
                 pubsub_itc_fw::ConfigurationException);
}

// Every field but enabled is required. Defaulting a host or port would give a sequencer
// that quietly dials the wrong place, which is worse than one that refuses to start.
TEST(SequencerConfigurationLoaderTest, RejectsAnEntryMissingItsPort) {
    EXPECT_THROW(load_with_gateways(R"(
[[gateway]]
protocol = 1
instance = 1
host     = "10.0.0.1"
)"),
                 pubsub_itc_fw::ConfigurationException);
}
