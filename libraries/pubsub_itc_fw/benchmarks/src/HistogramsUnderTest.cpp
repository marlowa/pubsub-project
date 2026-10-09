// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include <HistogramsUnderTest.hpp>

#include <SingleWriterHistogramThroughInterface.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/MetricKey.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

namespace pubsub_itc_fw_benchmarks {

namespace {

// The bounds of the venue's latency histograms in nanoseconds, from 100 nanoseconds to 100
// milliseconds: eighteen bounds and a final bucket for everything above them. The number of bounds
// matters, because recording a value starts with a search of them.
const std::vector<double>& bucket_upper_bounds() {
    static const std::vector<double> bounds = {
        100.0,    250.0,    500.0,    1000.0,    2500.0,    5000.0,    10000.0,    25000.0,    50000.0,
        100000.0, 250000.0, 500000.0, 1000000.0, 2500000.0, 5000000.0, 10000000.0, 50000000.0, 100000000.0,
    };
    return bounds;
}

pubsub_itc_fw::MetricsConfiguration enabled_metrics() {
    pubsub_itc_fw::MetricsConfiguration configuration;
    configuration.enabled = true;
    configuration.application = "benchmark";
    configuration.component = "prometheus_observe_overhead";
    configuration.listen_endpoint.host = "127.0.0.1";
    configuration.listen_endpoint.port = 0;
    return configuration;
}

} // un-named namespace

pubsub_itc_fw::HistogramHandle concrete_histogram_handle() {
    static pubsub_itc_fw::PrometheusEndpoint endpoint(enabled_metrics());
    static const pubsub_itc_fw::HistogramHandle handle =
        endpoint.register_histogram(pubsub_itc_fw::MetricKey("benchmark.prometheus_observe_overhead.observe_nanoseconds"),
                                    "Values recorded by the benchmark through the concrete histogram handle", bucket_upper_bounds());
    return handle;
}

InterfaceHistogramHandle interface_histogram_handle() {
    static SingleWriterHistogramThroughInterface histogram(bucket_upper_bounds());
    return InterfaceHistogramHandle(&histogram);
}

} // namespaces
