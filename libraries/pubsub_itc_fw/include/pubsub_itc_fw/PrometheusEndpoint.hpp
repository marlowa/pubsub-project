#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// prometheus-cpp is version 1.3.0, released 2024-11-03.
// Not to be confused with prometheus, which is a large system that requires go.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <pubsub_itc_fw/CounterHandle.hpp>
#include <pubsub_itc_fw/CounterInterface.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/GaugeInterface.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/HistogramInterface.hpp>
#include <pubsub_itc_fw/MetricKey.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/PrometheusCounter.hpp>
#include <pubsub_itc_fw/PrometheusGauge.hpp>
#include <pubsub_itc_fw/PrometheusHistogram.hpp>

namespace prometheus {
class Exposer;
} // namespaces

namespace pubsub_itc_fw {

/**
 * @brief This process's metrics: registration, and the HTTP endpoint Prometheus scrapes.
 *
 * See docs/operations/metrics.md for the whole picture. The essentials:
 *
 * A MetricKey names a metric as `<application>.<component>[.<scope>].<metricName>`. The
 * leaf becomes the Prometheus metric name and the rest become labels, so metrics sharing a
 * leaf name form one family with several labelled children.
 *
 * `register_*` hands back a reference to an interface, never to a prometheus-cpp type, so
 * no call site knows whether metrics are switched on. When they are not, every metric is a
 * shared no-op.
 *
 * ### Construction, then start
 *
 * These are deliberately separate, and the reason is CPU affinity. The listener's threads
 * inherit the affinity of whichever thread calls `start()`, and civetweb's threads must not
 * run on hot-path cores.
 *
 * Ordering alone does not achieve that, which was established by measurement rather than
 * reasoning: the Reactor calls this after the CPU layout has been applied, and at that
 * point the calling thread is pinned to its own hot-path core, so the listener threads
 * inherited it. Every civetweb thread of one gateway was found sharing core 3 with that
 * gateway's reactor thread.
 *
 * The caller must therefore widen its own affinity to the background cores across this
 * call and restore it afterwards, which is what Reactor::start_metrics_endpoint() does.
 * Separating construction from starting is what gives the caller a point at which to do
 * so -- a constructor would have to be handed the core list, which is not known until the
 * layout has been read.
 *
 * ### Lifetime
 *
 * Every reference returned by `register_*` points into the registry this object owns, so
 * **this object must outlive every holder**. The Reactor owns it for that reason.
 *
 * ### Threading
 *
 * Registration is mutex-guarded and is expected at startup rather than on a hot path.
 * Recording is not guarded here: Counter and Gauge are atomic, and Histogram takes its own
 * lock, which is needed regardless of how the application divides metrics between threads
 * because the scrape thread reads the same objects.
 */
class PrometheusEndpoint {
  public:
    ~PrometheusEndpoint();

    /** @param[in] configuration Whether metrics are on, and where the listener binds. */
    explicit PrometheusEndpoint(const MetricsConfiguration& configuration);

    PrometheusEndpoint(const PrometheusEndpoint&) = delete;
    PrometheusEndpoint& operator=(const PrometheusEndpoint&) = delete;
    PrometheusEndpoint(PrometheusEndpoint&&) = delete;
    PrometheusEndpoint& operator=(PrometheusEndpoint&&) = delete;

    /**
     * @brief Starts serving scrapes. Does nothing when metrics are disabled.
     *
     * Must be called after the CPU layout has been applied, so the listener's threads
     * inherit the background-core mask; see the note above. Calling it twice raises
     * PreconditionAssertion.
     *
     * Raises PubSubItcException if the listener cannot bind, which is a deployment fault
     * worth failing on rather than running blind without metrics.
     */
    void start();

    /** @brief Whether metrics are collected and served. */
    [[nodiscard]] bool enabled() const {
        return configuration_.enabled;
    }

    /**
     * @brief The port actually bound, or 0 when not started or disabled.
     *
     * Configuring port 0 asks the operating system to choose, so this is the only way to
     * learn what it chose. Logged at startup, and used by tests to reach the endpoint.
     */
    [[nodiscard]] uint16_t listening_port() const;

    /**
     * @brief Renders every metric in the Prometheus text format, exactly as a scrape of
     *        this process would return it right now.
     *
     * "Exposition format" is Prometheus's term for the plain-text body its server fetches
     * over HTTP. It is a line-based rendering of the current value of every metric, with
     * two comment lines introducing each family:
     *
     *     # HELP orders_total Orders accepted
     *     # TYPE orders_total counter
     *     orders_total{application="pubsub",component="gateway",scope="fix"} 12
     *     orders_total{application="pubsub",component="gateway",scope="binary"} 7
     *
     * HELP carries the description; TYPE says counter, gauge or histogram; each remaining
     * line is one time series -- a metric name, its labels, and its value. A histogram
     * renders as several lines per series: one `_bucket` line per boundary, plus `_sum`
     * and `_count`.
     *
     * There is nothing to parse or interpret here; the text is the wire format, and what
     * this returns is byte-for-byte what a scrape gets.
     *
     * Present for tests, which assert on this real rendering rather than on a fake, since
     * the mapping from a metric key to the rendered time series is the whole point of the
     * class. Returns an empty string when metrics are disabled, because nothing is
     * collected at all in that case.
     */
    [[nodiscard]] std::string exposition_text() const;

    /**
     * @brief Registers a counter and returns the handle to record through.
     *
     * @param[in] metric_key Identifies the metric and supplies its labels.
     * @param[in] help       Help text for the family. Must be identical for every key
     *                       sharing a metric name, since Prometheus allows one per family.
     *
     * Raises PreconditionAssertion if this exact key is registered twice, or if a metric
     * name is registered with help text differing from its first registration. Both are
     * programming errors, and both are checked whether or not metrics are enabled -- a
     * mistake that only appears once someone switches metrics on is worse than one that
     * appears immediately.
     */
    CounterHandle register_counter(const MetricKey& metric_key, const char* help);

    /** @brief As register_counter, for a gauge. */
    GaugeHandle register_gauge(const MetricKey& metric_key, const char* help);

    /**
     * @brief As register_counter, for a histogram.
     * @param[in] metric_key Identifies the metric and supplies its labels.
     * @param[in] help       Help text for the family; see register_counter.
     * @param[in] buckets    Upper bounds, ascending. Per child, so two scopes of one metric
     *                       may use different bucket sets.
     */
    HistogramHandle register_histogram(const MetricKey& metric_key, const char* help, const std::vector<double>& buckets);

    /**
     * @brief Registers a counter under a key composed from this process's identity.
     *
     * The application and component tokens come from configuration, so a caller supplies
     * only the parts that vary between metrics. Framework code needs this: the Reactor
     * declares the same metric in every component, so it cannot name any one of them, and
     * a literal key would be wrong in fourteen processes out of fifteen.
     *
     * It is also the better spelling for application code, because the two tokens are
     * process-wide -- repeating them at each call site is how one metric ends up saying
     * "gateway" while its neighbour says "fix_order_gateway".
     *
     * @param[in] scope       Third token, or empty for a key with no scope label.
     * @param[in] metric_name The metric name itself.
     * @param[in] help        As the MetricKey overload.
     */
    CounterHandle register_counter(const char* scope, const char* metric_name, const char* help);

    /** @brief As the three-argument register_counter, for a gauge. */
    GaugeHandle register_gauge(const char* scope, const char* metric_name, const char* help);

    /** @brief As the three-argument register_counter, for a histogram. */
    HistogramHandle register_histogram(const char* scope, const char* metric_name, const char* help, const std::vector<double>& buckets);

    /**
     * @brief The key the three-argument register_* calls would build.
     *
     * Exposed so a caller can hold or log a key without registering it, and so tests can
     * check what this process's identity composes to.
     */
    [[nodiscard]] MetricKey key_for(const char* scope, const char* metric_name) const;

  private:
    // Records the registration and enforces the two rules that apply whether or not
    // metrics are enabled. Returns the label set for the metric.
    std::map<std::string, std::string> note_registration(const MetricKey& metric_key, const char* help);

    const MetricsConfiguration configuration_;

    mutable std::mutex mutex_;

    // Every key registered, and the help text first seen for each metric name. Kept even
    // when disabled, so the rules above hold in every configuration.
    std::map<std::string, std::string> registered_help_by_metric_name_;
    std::map<std::string, bool> registered_keys_;

    // Null when metrics are disabled; nothing else is created in that case either.
    std::shared_ptr<prometheus::Registry> registry_;

    std::map<std::string, prometheus::Family<prometheus::Counter>*> counter_families_;
    std::map<std::string, prometheus::Family<prometheus::Gauge>*> gauge_families_;
    std::map<std::string, prometheus::Family<prometheus::Histogram>*> histogram_families_;

    // std::map is used rather than unordered_map because references to elements must stay
    // valid as more are inserted: register_* hands those references to callers that keep
    // them for the life of the process.
    std::map<std::string, PrometheusCounter> counters_;
    std::map<std::string, PrometheusGauge> gauges_;
    std::map<std::string, PrometheusHistogram> histograms_;

    // Declared last so it is destroyed FIRST. The listener collects from the registry, so
    // a scrape in flight while the registry is being torn down would read freed memory.
    // Member destruction runs in reverse declaration order, which makes this ordering the
    // guarantee -- it is not incidental field placement.
    std::unique_ptr<prometheus::Exposer> exposer_;
};

} // namespaces
