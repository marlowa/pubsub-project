// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "GatewayMetrics.hpp"

namespace {

// Loads a [metrics] section holding the given bucket literal, as deploy.py would produce it
// after expanding the shared placeholder into a gateway's file.
//
// Fills a caller-owned object rather than returning one: TomlConfiguration deletes its copy
// constructor, which suppresses the implicit move too, so it cannot be returned by value.
void load_with_buckets(pubsub_itc_fw::TomlConfiguration& configuration, const std::string& bucket_literal) {
    const std::string text = "[metrics]\norder_round_trip_buckets = " + bucket_literal + "\n";
    auto [ok, error] = configuration.load_string(text);
    ASSERT_TRUE(ok) << error;
}

TEST(GatewayMetricsTest, ReadsAscendingIntegerBounds) {
    // The spelling the environment files use: bare integers, no trailing .0.
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[10000, 25000, 50000, 100000]");
    const std::vector<double> buckets = gateway_metrics::load_order_round_trip_buckets(configuration);

    ASSERT_EQ(buckets.size(), 4u);
    EXPECT_DOUBLE_EQ(buckets.front(), 10000.0);
    EXPECT_DOUBLE_EQ(buckets.back(), 100000.0);
}

TEST(GatewayMetricsTest, RejectsEmptyBounds) {
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[]");
    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, RejectsDescendingBounds) {
    // Prometheus requires ascending bounds and misbehaves silently otherwise, so this has
    // to fail at load time rather than produce a histogram nobody can read.
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[50000, 25000, 10000]");
    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, RejectsARepeatedBound) {
    // Strictly ascending, not merely non-descending: a repeated bound yields an empty
    // bucket that can never be observed into.
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[10000, 25000, 25000]");
    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, RejectsAnInfiniteTopBound) {
    // The plausible-looking mistake, since every rendered histogram ends in le="+Inf".
    // prometheus-cpp adds that bucket itself, so declaring one produces a duplicate.
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[10000, 25000, inf]");
    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, RejectsANotANumberBound) {
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[10000, nan, 50000]");
    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, RejectsAMissingKey) {
    pubsub_itc_fw::TomlConfiguration configuration;
    auto [ok, error] = configuration.load_string("[metrics]\nenabled = true\n");
    ASSERT_TRUE(ok) << error;

    EXPECT_THROW(gateway_metrics::load_order_round_trip_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

TEST(GatewayMetricsTest, TheErrorNamesTheOffendingElement) {
    // There are a dozen bounds; an error that named only the key would leave the
    // operator comparing them by eye.
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[10000, 25000, 20000]");
    try {
        // The result is discarded, but the loader is [[nodiscard]], so it is bound.
        const std::vector<double> unused = gateway_metrics::load_order_round_trip_buckets(configuration);
        FAIL() << "expected ConfigurationException, got " << unused.size() << " bucket(s)";
    } catch (const pubsub_itc_fw::ConfigurationException& exception) {
        const std::string message = exception.what();
        EXPECT_NE(message.find("element 2"), std::string::npos) << message;
        EXPECT_NE(message.find(gateway_metrics::order_round_trip_buckets_key), std::string::npos) << message;
    }
}

TEST(GatewayMetricsTest, TheMetricNameCarriesNoProtocol) {
    // Both gateways register this one family and are told apart by the component label.
    // A protocol word here would mean two families, which cannot be compared in one query
    // and would leave the shared bucket bounds with nothing to enforce.
    const std::string name = gateway_metrics::order_round_trip_metric_name;
    EXPECT_EQ(name.find("fix"), std::string::npos) << name;
    EXPECT_EQ(name.find("binary"), std::string::npos) << name;
    // Prometheus convention: a histogram is named for its unit, with no _total suffix.
    EXPECT_NE(name.find("nanoseconds"), std::string::npos) << name;
}

} // namespaces
