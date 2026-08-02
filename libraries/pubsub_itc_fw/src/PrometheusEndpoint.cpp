// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <exception>

#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>

#include <pubsub_itc_fw/MetricKey.hpp>
#include <pubsub_itc_fw/NoOpCounter.hpp>
#include <pubsub_itc_fw/NoOpGauge.hpp>
#include <pubsub_itc_fw/NoOpHistogram.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

namespace {

// The no-op metrics carry no state, so one of each serves every disabled metric in the
// process rather than one per registration. That is what makes the disabled path free of
// allocation as well as free of work.
NoOpCounter& shared_no_op_counter() {
    static NoOpCounter instance;
    return instance;
}

NoOpGauge& shared_no_op_gauge() {
    static NoOpGauge instance;
    return instance;
}

NoOpHistogram& shared_no_op_histogram() {
    static NoOpHistogram instance;
    return instance;
}

constexpr const char* application_label = "application";
constexpr const char* component_label = "component";
constexpr const char* scope_label = "scope";

} // un-named namespace

PrometheusEndpoint::PrometheusEndpoint(const MetricsConfiguration& configuration) : configuration_(configuration) {
    // Nothing at all is built when metrics are off -- no registry, and later no listener.
    // Registration still validates; see note_registration.
    if (configuration_.enabled) {
        registry_ = std::make_shared<prometheus::Registry>();
    }
}

// Out of line, and not defaulted in the header, because prometheus::Exposer is only
// forward declared there: unique_ptr needs the complete type at the point the destructor
// is generated.
PrometheusEndpoint::~PrometheusEndpoint() = default;

void PrometheusEndpoint::start() {
    if (!configuration_.enabled) {
        return;
    }
    if (exposer_ != nullptr) {
        throw PreconditionAssertion("PrometheusEndpoint::start: already started", __FILE__, __LINE__);
    }

    const std::string bind_address = fmt::format("{}:{}", configuration_.listen_endpoint.host, configuration_.listen_endpoint.port);

    // prometheus-cpp reports a bind failure by throwing. Translated rather than propagated
    // so the message names this endpoint and the address tried: the raw exception says
    // neither, and a metrics port clashing with another component's is exactly the kind of
    // deployment mistake that needs to say which port.
    try {
        exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
    } catch (const std::exception& exception) {
        throw PubSubItcException(fmt::format("PrometheusEndpoint: could not bind the metrics listener to {}: {}", bind_address, exception.what()));
    }

    exposer_->RegisterCollectable(registry_);
}

uint16_t PrometheusEndpoint::listening_port() const {
    if (exposer_ == nullptr) {
        return 0;
    }
    // A configured port of 0 means "let the operating system choose", so the configured
    // value cannot be reported back -- only the listener knows what it got.
    const std::vector<int> ports = exposer_->GetListeningPorts();
    if (ports.empty()) {
        return 0;
    }
    return static_cast<uint16_t>(ports.front());
}

std::string PrometheusEndpoint::exposition_text() const {
    if (registry_ == nullptr) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream stream;
    const prometheus::TextSerializer serializer;
    serializer.Serialize(stream, registry_->Collect());
    return stream.str();
}

MetricKey PrometheusEndpoint::key_for(const char* scope, const char* metric_name) const {
    return MetricKey::compose(configuration_.application, configuration_.component, scope != nullptr ? scope : "", metric_name != nullptr ? metric_name : "");
}

CounterHandle PrometheusEndpoint::register_counter(const char* scope, const char* metric_name, const char* help) {
    return register_counter(key_for(scope, metric_name), help);
}

GaugeHandle PrometheusEndpoint::register_gauge(const char* scope, const char* metric_name, const char* help) {
    return register_gauge(key_for(scope, metric_name), help);
}

HistogramHandle PrometheusEndpoint::register_histogram(const char* scope, const char* metric_name, const char* help, const std::vector<double>& buckets) {
    return register_histogram(key_for(scope, metric_name), help, buckets);
}

std::map<std::string, std::string> PrometheusEndpoint::note_registration(const MetricKey& metric_key, const char* help) {
    const std::string help_text = help != nullptr ? help : "";

    // Registering the same key twice is a programming error: the caller already holds a
    // reference and should keep it. Returning the existing metric instead would hide the
    // duplicate, and the two call sites would silently share one metric.
    if (registered_keys_.find(metric_key.full_name()) != registered_keys_.end()) {
        throw PreconditionAssertion(fmt::format("PrometheusEndpoint: metric '{}' is already registered", metric_key.full_name()), __FILE__, __LINE__);
    }

    // Sharing a metric name is normal and is the point of the labels -- two scopes of one
    // metric are one family with two children. What cannot differ is the help text, because
    // Prometheus allows one per family, so the second registration would be silently
    // discarded and the exposed help would depend on registration order.
    const auto existing_help = registered_help_by_metric_name_.find(metric_key.metric_name());
    if (existing_help != registered_help_by_metric_name_.end() && existing_help->second != help_text) {
        throw PreconditionAssertion(fmt::format("PrometheusEndpoint: metric name '{}' is already registered with different help text. "
                                                "Prometheus allows one help string per metric family, so every key sharing a name must "
                                                "supply the same text.\n  first: '{}'\n  now  : '{}'\n  key  : '{}'",
                                                metric_key.metric_name(), existing_help->second, help_text, metric_key.full_name()),
                                    __FILE__, __LINE__);
    }

    registered_keys_.emplace(metric_key.full_name(), true);
    registered_help_by_metric_name_.emplace(metric_key.metric_name(), help_text);

    std::map<std::string, std::string> labels;
    labels[application_label] = metric_key.application();
    labels[component_label] = metric_key.component();
    // Omitted rather than emitted empty when the key has no scope: an absent label and an
    // empty one match differently in a query such as {scope=~".+"}.
    if (metric_key.has_scope()) {
        labels[scope_label] = metric_key.scope();
    }
    return labels;
}

CounterHandle PrometheusEndpoint::register_counter(const MetricKey& metric_key, const char* help) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::map<std::string, std::string> labels = note_registration(metric_key, help);
    if (!configuration_.enabled) {
        return CounterHandle(&shared_no_op_counter());
    }

    const std::string& name = metric_key.metric_name();
    auto family = counter_families_.find(name);
    if (family == counter_families_.end()) {
        auto& built = prometheus::BuildCounter().Name(name).Help(help != nullptr ? help : "").Register(*registry_);
        family = counter_families_.emplace(name, &built).first;
    }

    prometheus::Counter& counter = family->second->Add(labels);
    return CounterHandle(&counters_.emplace(metric_key.full_name(), PrometheusCounter(&counter)).first->second);
}

GaugeHandle PrometheusEndpoint::register_gauge(const MetricKey& metric_key, const char* help) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::map<std::string, std::string> labels = note_registration(metric_key, help);
    if (!configuration_.enabled) {
        return GaugeHandle(&shared_no_op_gauge());
    }

    const std::string& name = metric_key.metric_name();
    auto family = gauge_families_.find(name);
    if (family == gauge_families_.end()) {
        auto& built = prometheus::BuildGauge().Name(name).Help(help != nullptr ? help : "").Register(*registry_);
        family = gauge_families_.emplace(name, &built).first;
    }

    prometheus::Gauge& gauge = family->second->Add(labels);
    return GaugeHandle(&gauges_.emplace(metric_key.full_name(), PrometheusGauge(&gauge)).first->second);
}

HistogramHandle PrometheusEndpoint::register_histogram(const MetricKey& metric_key, const char* help, const std::vector<double>& buckets) {
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::map<std::string, std::string> labels = note_registration(metric_key, help);
    if (!configuration_.enabled) {
        return HistogramHandle(&shared_no_op_histogram());
    }

    const std::string& name = metric_key.metric_name();
    auto family = histogram_families_.find(name);
    if (family == histogram_families_.end()) {
        auto& built = prometheus::BuildHistogram().Name(name).Help(help != nullptr ? help : "").Register(*registry_);
        family = histogram_families_.emplace(name, &built).first;
    }

    prometheus::Histogram& histogram = family->second->Add(labels, buckets);
    return HistogramHandle(&histograms_.emplace(metric_key.full_name(), PrometheusHistogram(&histogram)).first->second);
}

} // namespaces
