// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MetricKey.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

using pubsub_itc_fw::MetricKey;
using pubsub_itc_fw::MetricsConfiguration;
using pubsub_itc_fw::PreconditionAssertion;
using pubsub_itc_fw::PrometheusEndpoint;

namespace {

// Port 0 asks the operating system for an ephemeral port. Every test that starts a listener
// uses it, so the suite never collides with a fixed port -- either with another test running
// in parallel or with a component already running on the developer's machine.
constexpr uint16_t ephemeral_port = 0;

MetricsConfiguration enabled_configuration() {
    MetricsConfiguration configuration;
    configuration.enabled = true;
    configuration.listen_endpoint.host = "127.0.0.1";
    configuration.listen_endpoint.port = ephemeral_port;
    return configuration;
}

MetricsConfiguration disabled_configuration() {
    MetricsConfiguration configuration;
    configuration.enabled = false;
    return configuration;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // un-named namespace

// -- What a scrape actually renders --------------------------------------------
//
// These assert on the real exposition text rather than on a fake, because the whole point
// of the class is the mapping from a dotted key to a Prometheus time series. A fake would
// only re-state the mapping the test is meant to check.

TEST(PrometheusEndpointTest, RendersTheKeyAsAMetricNameAndThreeLabels) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const MetricKey key("pubsub.gateway.binary.orders_total");

    endpoint.register_counter(key, "Orders accepted").increment();

    const std::string exposition = endpoint.exposition_text();
    EXPECT_TRUE(contains(exposition, "# HELP orders_total Orders accepted")) << exposition;
    EXPECT_TRUE(contains(exposition, "# TYPE orders_total counter")) << exposition;
    EXPECT_TRUE(contains(exposition, "application=\"pubsub\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "component=\"gateway\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "scope=\"binary\"")) << exposition;
}

// A three-token key must produce no scope label at all. An empty label value is not the
// same thing: {scope=~".+"} selects one and not the other.
TEST(PrometheusEndpointTest, OmitsTheScopeLabelWhenTheKeyHasNoScope) {
    PrometheusEndpoint endpoint(enabled_configuration());

    endpoint.register_counter(MetricKey("pubsub.gateway.orders_total"), "Orders accepted").increment();

    const std::string exposition = endpoint.exposition_text();
    EXPECT_TRUE(contains(exposition, "application=\"pubsub\"")) << exposition;
    EXPECT_FALSE(contains(exposition, "scope=")) << "a scope label was emitted for a key with no scope:\n" << exposition;
}

// The reason two keys may share a metric name: one family, several labelled children. If
// this ever produced two families the exposition would carry a duplicate HELP line, which
// Prometheus rejects on ingestion.
TEST(PrometheusEndpointTest, TwoKeysSharingAMetricNameBecomeOneFamilyWithTwoChildren) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const std::string help = "Orders accepted";

    endpoint.register_counter(MetricKey("pubsub.gateway.fix.orders_total"), help.c_str()).increment();
    endpoint.register_counter(MetricKey("pubsub.gateway.binary.orders_total"), help.c_str()).increment();

    const std::string exposition = endpoint.exposition_text();

    size_t help_lines = 0;
    size_t search_from = 0;
    const std::string help_marker = "# HELP orders_total";
    while ((search_from = exposition.find(help_marker, search_from)) != std::string::npos) {
        ++help_lines;
        search_from += help_marker.size();
    }
    EXPECT_EQ(help_lines, 1U) << "expected a single family for one metric name:\n" << exposition;

    EXPECT_TRUE(contains(exposition, "scope=\"fix\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "scope=\"binary\"")) << exposition;
}

TEST(PrometheusEndpointTest, RecordsCounterGaugeAndHistogramValues) {
    PrometheusEndpoint endpoint(enabled_configuration());

    auto& counter = endpoint.register_counter(MetricKey("pubsub.gateway.orders_total"), "Orders");
    auto& gauge = endpoint.register_gauge(MetricKey("pubsub.gateway.sessions_open"), "Open sessions");
    auto& histogram = endpoint.register_histogram(MetricKey("pubsub.gateway.latency_seconds"), "Latency", {0.001, 0.01, 0.1});

    counter.increment();
    counter.increment();
    gauge.set(7);
    histogram.observe(0.005);
    histogram.observe(0.5);

    const std::string exposition = endpoint.exposition_text();
    EXPECT_TRUE(contains(exposition, "orders_total{")) << exposition;
    EXPECT_TRUE(contains(exposition, "} 2")) << "counter did not reach 2:\n" << exposition;
    EXPECT_TRUE(contains(exposition, "sessions_open{")) << exposition;
    EXPECT_TRUE(contains(exposition, "} 7")) << "gauge did not read 7:\n" << exposition;
    EXPECT_TRUE(contains(exposition, "latency_seconds_bucket")) << exposition;
    EXPECT_TRUE(contains(exposition, "latency_seconds_count")) << exposition;
    // Two observations, one of which is above every bucket bound, so +Inf holds both.
    EXPECT_TRUE(contains(exposition, "le=\"+Inf\"")) << exposition;
}

// Buckets are per child, so one metric name may carry different bucket sets for different
// scopes. That is a property of the Family::Add call and worth pinning down.
TEST(PrometheusEndpointTest, TwoScopesOfOneHistogramMayUseDifferentBuckets) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const std::string help = "Latency";

    endpoint.register_histogram(MetricKey("pubsub.gateway.fix.latency_seconds"), help.c_str(), {0.1}).observe(0.05);
    endpoint.register_histogram(MetricKey("pubsub.gateway.binary.latency_seconds"), help.c_str(), {0.001, 0.002}).observe(0.0005);

    const std::string exposition = endpoint.exposition_text();
    EXPECT_TRUE(contains(exposition, "le=\"0.1\"")) << exposition;
    EXPECT_TRUE(contains(exposition, "le=\"0.002\"")) << exposition;
}

// -- Disabled ------------------------------------------------------------------

TEST(PrometheusEndpointTest, DisabledRegistersNothingAndExposesNothing) {
    PrometheusEndpoint endpoint(disabled_configuration());

    endpoint.register_counter(MetricKey("pubsub.gateway.orders_total"), "Orders").increment();
    endpoint.register_gauge(MetricKey("pubsub.gateway.sessions_open"), "Sessions").set(3);
    endpoint.register_histogram(MetricKey("pubsub.gateway.latency_seconds"), "Latency", {0.1}).observe(0.05);

    EXPECT_FALSE(endpoint.enabled());
    EXPECT_EQ(endpoint.exposition_text(), "");
}

// Recording through a disabled metric must be harmless, not merely unrecorded -- these are
// the calls that will litter the hot path once components are instrumented.
TEST(PrometheusEndpointTest, DisabledMetricsAreSafeToRecordThrough) {
    PrometheusEndpoint endpoint(disabled_configuration());
    auto& counter = endpoint.register_counter(MetricKey("pubsub.gateway.orders_total"), "Orders");

    for (int index = 0; index < 1000; ++index) {
        counter.increment();
    }

    EXPECT_EQ(endpoint.exposition_text(), "");
}

// start() on a disabled endpoint is a no-op rather than an error: a component calls it
// unconditionally, and having to ask whether metrics are on before starting them would put
// the switch back at the call site.
TEST(PrometheusEndpointTest, StartingADisabledEndpointDoesNothing) {
    PrometheusEndpoint endpoint(disabled_configuration());

    endpoint.start();

    EXPECT_EQ(endpoint.listening_port(), 0);
}

// -- The listener --------------------------------------------------------------

TEST(PrometheusEndpointTest, StartBindsAnEphemeralPortAndReportsIt) {
    PrometheusEndpoint endpoint(enabled_configuration());

    EXPECT_EQ(endpoint.listening_port(), 0) << "a port was reported before start()";
    endpoint.start();

    EXPECT_NE(endpoint.listening_port(), 0) << "port 0 should have been replaced by the one the OS assigned";
}

// Starting twice is a programming error rather than something to absorb: the second call
// would otherwise leak a listener and leave two bound ports.
TEST(PrometheusEndpointTest, StartingTwiceRaisesAPrecondition) {
    PrometheusEndpoint endpoint(enabled_configuration());
    endpoint.start();

    EXPECT_THROW(endpoint.start(), PreconditionAssertion);
}

// Metrics registered before the listener starts must still be served, since registration
// happens during construction and start() is deliberately deferred until after the CPU
// layout has been applied.
TEST(PrometheusEndpointTest, MetricsRegisteredBeforeStartAreStillExposed) {
    PrometheusEndpoint endpoint(enabled_configuration());
    endpoint.register_counter(MetricKey("pubsub.gateway.orders_total"), "Orders").increment();

    endpoint.start();

    EXPECT_TRUE(contains(endpoint.exposition_text(), "orders_total")) << endpoint.exposition_text();
}

// -- Registration rules --------------------------------------------------------

TEST(PrometheusEndpointTest, RegisteringTheSameKeyTwiceRaisesAPrecondition) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const MetricKey key("pubsub.gateway.orders_total");
    endpoint.register_counter(key, "Orders");

    EXPECT_THROW(endpoint.register_counter(key, "Orders"), PreconditionAssertion);
}

// A key is one metric whatever its type, so re-registering it as a different type is the
// same mistake and must be caught the same way.
TEST(PrometheusEndpointTest, RegisteringOneKeyUnderTwoTypesRaisesAPrecondition) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const MetricKey key("pubsub.gateway.orders_total");
    endpoint.register_counter(key, "Orders");

    EXPECT_THROW(endpoint.register_gauge(key, "Orders"), PreconditionAssertion);
}

// Prometheus allows one help string per family, so the second registration would be
// silently discarded and the exposed text would depend on registration order.
TEST(PrometheusEndpointTest, SameMetricNameWithDifferentHelpRaisesAPrecondition) {
    PrometheusEndpoint endpoint(enabled_configuration());
    endpoint.register_counter(MetricKey("pubsub.gateway.fix.orders_total"), "Orders accepted");

    EXPECT_THROW(endpoint.register_counter(MetricKey("pubsub.gateway.binary.orders_total"), "A different description"), PreconditionAssertion);
}

TEST(PrometheusEndpointTest, SameMetricNameWithIdenticalHelpIsAccepted) {
    PrometheusEndpoint endpoint(enabled_configuration());
    const std::string help = "Orders accepted";

    endpoint.register_counter(MetricKey("pubsub.gateway.fix.orders_total"), help.c_str());

    EXPECT_NO_THROW(endpoint.register_counter(MetricKey("pubsub.gateway.binary.orders_total"), help.c_str()));
}

// The rules must not depend on whether metrics happen to be switched on: a duplicate
// registration that only fails in the environment with metrics enabled is a bug waiting for
// the worst possible moment to appear.
TEST(PrometheusEndpointTest, RegistrationRulesApplyWhenDisabledToo) {
    PrometheusEndpoint endpoint(disabled_configuration());
    const MetricKey key("pubsub.gateway.orders_total");
    endpoint.register_counter(key, "Orders");

    EXPECT_THROW(endpoint.register_counter(key, "Orders"), PreconditionAssertion);
    EXPECT_THROW(endpoint.register_counter(MetricKey("pubsub.gateway.fix.orders_total"), "Different"), PreconditionAssertion);
}

// -- Handle stability ----------------------------------------------------------

// Callers keep the reference for the life of the process, so it must survive later
// registrations. This is why the maps are std::map: node-based containers keep references
// to elements valid across inserts, which unordered_map with rehashing would too, but a
// vector would not.
TEST(PrometheusEndpointTest, HandlesStayValidAsMoreMetricsAreRegistered) {
    PrometheusEndpoint endpoint(enabled_configuration());
    auto& first = endpoint.register_counter(MetricKey("pubsub.gateway.first_total"), "First");

    for (int index = 0; index < 200; ++index) {
        // MetricKey takes const char*, so a key built at run time needs c_str(). Keys are
        // literals at real call sites; only a test generates them in a loop.
        const std::string filler_key = "pubsub.gateway.scope" + std::to_string(index) + ".filler_total";
        endpoint.register_counter(MetricKey(filler_key.c_str()), "Filler");
    }

    first.increment();

    EXPECT_TRUE(contains(endpoint.exposition_text(), "first_total")) << endpoint.exposition_text();
}
