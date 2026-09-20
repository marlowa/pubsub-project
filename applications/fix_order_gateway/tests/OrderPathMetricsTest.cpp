// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for the timings every component on the order path records.
//
// The two rules about when NOT to record are what these tests are mostly for. Both guard
// against a number that is wrong rather than against a crash, and a wrong number recorded
// here cannot be spotted or undone later: Prometheus keeps a running total per histogram, so
// a single bad observation moves every average drawn from it from then on, and there is no
// query that can take it back out.
//
// They record through a real PrometheusEndpoint and read its exposition text, rather than
// through a stub, because what is being checked is that an observation reached the histogram
// and carried the right value. A stub would only confirm that the function called what the
// test author expected it to call.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "OrderPathMetrics.hpp"

namespace {

// Port 0 asks the operating system to choose one, so tests never collide on a fixed port.
constexpr uint16_t ephemeral_port = 0;

pubsub_itc_fw::MetricsConfiguration enabled_configuration() {
    pubsub_itc_fw::MetricsConfiguration configuration;
    configuration.enabled = true;
    configuration.listen_endpoint.host = "127.0.0.1";
    configuration.listen_endpoint.port = ephemeral_port;
    return configuration;
}

// Bounds wide enough that any value these tests record lands somewhere sensible. What the
// tests read back is the count and the running total, neither of which depends on where the
// boundaries fall.
const std::vector<double> test_buckets = {1000.0, 10000.0, 100000.0, 1000000.0};

// What one histogram holds: how many timings were recorded, and their total.
//
// Read out of the exposition by name rather than by matching text anywhere in it. A bucket
// line ends in a count too, so a test that merely looked for "} 1" somewhere would pass
// whether or not anything had been recorded, and would go on passing after the guard it is
// meant to be checking had been deleted.
struct Recorded {
    double count{-1.0};
    double total{-1.0};
};

double value_on_line_beginning(const std::string& exposition, const std::string& prefix) {
    for (size_t start = 0; start < exposition.size();) {
        const size_t end = exposition.find('\n', start);
        const std::string line = exposition.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.rfind(prefix, 0) == 0) {
            const size_t space = line.rfind(' ');
            if (space != std::string::npos) {
                return std::stod(line.substr(space + 1));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return -1.0;
}

// Records one timing through a real histogram and reports what that histogram then holds.
Recorded record(bool has_origin, int64_t origin_ns, int64_t now_ns) {
    pubsub_itc_fw::PrometheusEndpoint endpoint(enabled_configuration());
    pubsub_itc_fw::HistogramHandle histogram = endpoint.register_histogram(
        order_path_metrics::order_in_scope, order_path_metrics::order_path_elapsed_metric_name, order_path_metrics::order_path_elapsed_help, test_buckets);

    order_path_metrics::observe_checkpoint(histogram, has_origin, origin_ns, now_ns);

    const std::string exposition = endpoint.exposition_text();
    Recorded recorded;
    recorded.count = value_on_line_beginning(exposition, "order_path_elapsed_nanoseconds_count{");
    recorded.total = value_on_line_beginning(exposition, "order_path_elapsed_nanoseconds_sum{");
    EXPECT_GE(recorded.count, 0.0) << "the histogram was not registered at all:\n" << exposition;
    return recorded;
}

// Loads a [metrics] section holding the given bucket literal, as deploy.py would produce it
// after expanding the shared value into a component's file.
//
// Fills a caller-owned object rather than returning one: TomlConfiguration deletes its copy
// constructor, which suppresses the implicit move too, so it cannot be returned by value.
void load_with_buckets(pubsub_itc_fw::TomlConfiguration& configuration, const std::string& bucket_literal) {
    const std::string text = "[metrics]\norder_path_elapsed_buckets = " + bucket_literal + "\n";
    auto [ok, error] = configuration.load_string(text);
    ASSERT_TRUE(ok) << error;
}

TEST(OrderPathMetricsTest, RecordsHowLongTheOrderHadBeenInTheVenue) {
    const Recorded recorded = record(true, 1'000'000'000, 1'000'050'000);

    EXPECT_DOUBLE_EQ(recorded.count, 1.0);
    EXPECT_DOUBLE_EQ(recorded.total, 50000.0) << "the value recorded should be the difference between the two times";
}

TEST(OrderPathMetricsTest, RecordsNothingWhenThereIsNoStartTime) {
    // A record one sequencer sends another, a report produced with no client order behind it,
    // or an order replayed from the log: none of these has a start time. Treating the absent
    // value as zero would record the time since 1970 and drag every later average with it.
    const Recorded recorded = record(false, 0, 1'000'050'000);

    EXPECT_DOUBLE_EQ(recorded.count, 0.0) << "nothing should have been recorded without a start time";
    EXPECT_DOUBLE_EQ(recorded.total, 0.0);
}

TEST(OrderPathMetricsTest, RecordsNothingWhenTheStartTimeIsInTheFuture) {
    // After a gateway fails over, the two ends of the measurement are taken by two different
    // processes whose clocks need not agree to the nanosecond, so a negative elapsed time is
    // possible and is not a very fast order.
    const Recorded recorded = record(true, 1'000'050'000, 1'000'000'000);

    EXPECT_DOUBLE_EQ(recorded.count, 0.0) << "a negative elapsed time should not have been recorded";
    EXPECT_DOUBLE_EQ(recorded.total, 0.0) << "a negative value in the total would bias every average drawn from it";
}

TEST(OrderPathMetricsTest, RecordsAnElapsedTimeOfNothingRatherThanDiscardingIt) {
    // Zero is a real reading, not a failed one: the two ends can fall in the same nanosecond.
    // Only a value below zero is impossible, so only that is thrown away.
    const Recorded recorded = record(true, 1'000'000'000, 1'000'000'000);

    EXPECT_DOUBLE_EQ(recorded.count, 1.0) << "an elapsed time of zero is a reading, not a failure";
    EXPECT_DOUBLE_EQ(recorded.total, 0.0);
}

TEST(OrderPathMetricsTest, ReadsItsOwnBucketBounds) {
    pubsub_itc_fw::TomlConfiguration configuration;
    load_with_buckets(configuration, "[1000, 10000, 100000]");
    const std::vector<double> buckets = order_path_metrics::load_order_path_elapsed_buckets(configuration);

    ASSERT_EQ(buckets.size(), 3u);
    EXPECT_DOUBLE_EQ(buckets.front(), 1000.0);
    EXPECT_DOUBLE_EQ(buckets.back(), 100000.0);
}

TEST(OrderPathMetricsTest, AppliesTheSameValidationAsEveryOtherHistogram) {
    // The rules live in one place so that no component on the path can be held to a laxer
    // standard than the one next to it. This checks the shared validator is really reached,
    // rather than this loader having quietly grown a copy of its own.
    pubsub_itc_fw::TomlConfiguration descending;
    load_with_buckets(descending, "[100000, 10000]");
    EXPECT_THROW(order_path_metrics::load_order_path_elapsed_buckets(descending), pubsub_itc_fw::ConfigurationException);

    pubsub_itc_fw::TomlConfiguration empty;
    load_with_buckets(empty, "[]");
    EXPECT_THROW(order_path_metrics::load_order_path_elapsed_buckets(empty), pubsub_itc_fw::ConfigurationException);
}

TEST(OrderPathMetricsTest, DoesNotFallBackToTheRoundTripBounds) {
    // The two are separate settings on purpose. A loader that read the wrong key would work
    // perfectly until the day the two were configured differently, and then would silently
    // measure the path against boundaries chosen for the whole round trip.
    pubsub_itc_fw::TomlConfiguration configuration;
    auto [ok, error] = configuration.load_string("[metrics]\norder_round_trip_buckets = [10000, 25000]\n");
    ASSERT_TRUE(ok) << error;

    EXPECT_THROW(order_path_metrics::load_order_path_elapsed_buckets(configuration), pubsub_itc_fw::ConfigurationException);
}

} // namespaces
