// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorBehaviourStatistics.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

#include "PoolMetricsReporter.hpp"

using fix_common::PoolMetricsReporter;
using pubsub_itc_fw::AllocatorBehaviourStatistics;
using pubsub_itc_fw::MetricsConfiguration;
using pubsub_itc_fw::PoolStatistics;
using pubsub_itc_fw::PrometheusEndpoint;

namespace {

// Port 0 asks the operating system for an ephemeral port, so the suite never collides with
// a fixed one -- neither with a parallel test nor with a component already running.
constexpr uint16_t ephemeral_port = 0;

MetricsConfiguration enabled_configuration() {
    MetricsConfiguration configuration;
    configuration.enabled = true;
    configuration.component = "binary_order_gateway_a";
    configuration.listen_endpoint.host = "127.0.0.1";
    configuration.listen_endpoint.port = ephemeral_port;
    return configuration;
}

// The gateway's real shape: 21 pools of 2^20 OpenOrderEntry, which is the case the
// arithmetic has to survive.
PoolStatistics gateway_shaped_pool() {
    PoolStatistics statistics;
    statistics.pool_name_ = "BinaryOpenOrderPool";
    statistics.object_size_ = 168U;
    statistics.number_of_objects_per_pool_ = 1048576;
    statistics.number_of_pools_ = 21;
    statistics.number_of_allocated_objects_ = 3000000;
    statistics.number_of_objects_available_ = 19020096;
    statistics.number_of_full_pools_ = 2;
    return statistics;
}

AllocatorBehaviourStatistics busy_allocator() {
    AllocatorBehaviourStatistics statistics;
    statistics.total_allocations = 9000000U;
    statistics.fast_path_allocations = 8999000U;
    statistics.slow_path_allocations = 1000U;
    statistics.expansion_events = 3U;
    statistics.failed_allocations = 0U;
    return statistics;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// The rendered value of a metric, found by name. Parsed as a double rather than matched as
// text because the exposition format's spelling of a number is prometheus-cpp's business,
// not this class's, and asserting on it would tie these tests to that library's formatting.
double rendered_value(const std::string& exposition, const std::string& metric_name) {
    size_t position = exposition.find("\n" + metric_name + "{");
    if (position == std::string::npos) {
        return -1.0;
    }
    const size_t closing_brace = exposition.find('}', position);
    if (closing_brace == std::string::npos) {
        return -1.0;
    }
    const size_t line_end = exposition.find('\n', closing_brace);
    return std::stod(exposition.substr(closing_brace + 1, line_end - closing_brace - 1));
}

} // un-named namespace

TEST(PoolMetricsReporterTest, IsNotRegisteredUntilRegisterMetricsIsCalled) {
    PoolMetricsReporter reporter;
    EXPECT_FALSE(reporter.is_registered());

    PrometheusEndpoint endpoint(enabled_configuration());
    reporter.register_metrics(endpoint, "open_order_pool");
    EXPECT_TRUE(reporter.is_registered());
}

// A reporter that has not been registered holds unbound handles, and those record nowhere.
// This matters because the gateway builds its pool in on_app_ready_event: anything that
// sampled earlier would otherwise have to know not to.
TEST(PoolMetricsReporterTest, UpdatingBeforeRegistrationDoesNothingAndDoesNotCrash) {
    PoolMetricsReporter reporter;
    reporter.update(gateway_shaped_pool(), busy_allocator());
    EXPECT_FALSE(reporter.is_registered());
}

// Registering the same key twice raises PreconditionAssertion inside the endpoint. The
// guard in register_metrics is what keeps a second call from being a crash.
TEST(PoolMetricsReporterTest, RegisteringTwiceIsHarmless) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;

    reporter.register_metrics(endpoint, "open_order_pool");
    EXPECT_NO_THROW(reporter.register_metrics(endpoint, "open_order_pool"));
    EXPECT_TRUE(reporter.is_registered());
}

TEST(PoolMetricsReporterTest, NamesThePoolWithTheScopeLabelRatherThanTheMetricName) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;
    reporter.register_metrics(endpoint, "open_order_pool");
    reporter.update(gateway_shaped_pool(), busy_allocator());

    const std::string exposition = endpoint.exposition_text();
    EXPECT_TRUE(contains(exposition, "# TYPE pool_bytes_reserved gauge")) << exposition;
    EXPECT_TRUE(contains(exposition, "scope=\"open_order_pool\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "component=\"binary_order_gateway_a\"")) << exposition;
    // The pool must not appear in the metric name, or one family per pool follows and with
    // it one panel and one alert per pool.
    EXPECT_FALSE(contains(exposition, "open_order_pool_bytes_reserved")) << exposition;
}

// Two pools in one process are two children of one family, not two families. This is the
// property that keeps a third pool costing a label value rather than thirteen new metrics.
TEST(PoolMetricsReporterTest, TwoPoolsShareOneFamily) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter open_orders;
    PoolMetricsReporter event_queue;

    open_orders.register_metrics(endpoint, "open_order_pool");
    event_queue.register_metrics(endpoint, "event_queue_pool");
    open_orders.update(gateway_shaped_pool(), busy_allocator());
    event_queue.update(gateway_shaped_pool(), busy_allocator());

    const std::string exposition = endpoint.exposition_text();
    size_t help_lines = 0;
    size_t position = exposition.find("# HELP pool_bytes_reserved");
    while (position != std::string::npos) {
        ++help_lines;
        position = exposition.find("# HELP pool_bytes_reserved", position + 1);
    }
    EXPECT_EQ(help_lines, 1U) << "expected one family for one metric name:\n" << exposition;
    EXPECT_TRUE(contains(exposition, "scope=\"open_order_pool\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "scope=\"event_queue_pool\"")) << exposition;
}

// The reason the widening in update() is there. 21 * 1048576 * 168 is 3,699,376,128, which
// overflows a signed 32-bit product even though every factor fits one comfortably. Computed
// in the int types PoolStatistics declares, this reads as a negative number.
TEST(PoolMetricsReporterTest, ReservedBytesSurvivesAPoolChainLargerThanThirtyTwoBits) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;
    reporter.register_metrics(endpoint, "open_order_pool");
    reporter.update(gateway_shaped_pool(), busy_allocator());

    const std::string exposition = endpoint.exposition_text();
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_bytes_reserved"), 3699376128.0);
    EXPECT_GT(rendered_value(exposition, "pool_bytes_reserved"), 2147483647.0);
}

// The pair that separates a pool's reservation from its live contents. A pool sized by
// initial_pools claims everything at startup, so resident memory cannot tell them apart.
TEST(PoolMetricsReporterTest, ReportsReservedAndInUseSeparately) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;
    reporter.register_metrics(endpoint, "open_order_pool");
    reporter.update(gateway_shaped_pool(), busy_allocator());

    const std::string exposition = endpoint.exposition_text();
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_bytes_in_use"), 3000000.0 * 168.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_objects_allocated"), 3000000.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_objects_available"), 19020096.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_segments"), 21.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_chain_full"), 2.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_object_size_bytes"), 168.0);
}

// The number worth alerting on: the allocator chains rather than falling back to the heap,
// so a pool configured too small keeps working and says so only here.
TEST(PoolMetricsReporterTest, PublishesTheBehaviourTotals) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;
    reporter.register_metrics(endpoint, "open_order_pool");
    reporter.update(gateway_shaped_pool(), busy_allocator());

    const std::string exposition = endpoint.exposition_text();
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_expansion_events"), 3.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_allocations"), 9000000.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_fast_path_allocations"), 8999000.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_slow_path_allocations"), 1000.0);
    EXPECT_DOUBLE_EQ(rendered_value(exposition, "pool_allocation_failures"), 0.0);
}

// A later snapshot replaces the earlier one. Gauges, not counters, so a value that falls --
// objects returned to the pool -- must be able to fall.
TEST(PoolMetricsReporterTest, ASecondUpdateReplacesTheFirst) {
    PrometheusEndpoint endpoint(enabled_configuration());
    PoolMetricsReporter reporter;
    reporter.register_metrics(endpoint, "open_order_pool");
    reporter.update(gateway_shaped_pool(), busy_allocator());

    PoolStatistics fewer = gateway_shaped_pool();
    fewer.number_of_allocated_objects_ = 5;
    reporter.update(fewer, busy_allocator());

    EXPECT_DOUBLE_EQ(rendered_value(endpoint.exposition_text(), "pool_objects_allocated"), 5.0);
}
