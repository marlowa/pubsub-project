// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>
#include <prometheus/metric_type.h>

#include <pubsub_itc_fw/SingleWriterHistogramRegistry.hpp>

#include <pubsub_itc_fw/SingleWriterHistogram.hpp>

namespace pubsub_itc_fw {

SingleWriterHistogram& SingleWriterHistogramRegistry::add(const std::string& metric_name, const std::string& help,
                                                          const std::map<std::string, std::string>& labels, const std::vector<double>& upper_bounds) {
    auto existing = family_index_by_name_.find(metric_name);
    if (existing == family_index_by_name_.end()) {
        Family family;
        family.metric_name = metric_name;
        family.help = help;
        families_.push_back(std::move(family));
        existing = family_index_by_name_.emplace(metric_name, families_.size() - 1).first;
    }

    Child child;
    child.labels = labels;
    child.histogram = std::make_unique<SingleWriterHistogram>(upper_bounds);
    SingleWriterHistogram& histogram = *child.histogram;
    families_[existing->second].children.push_back(std::move(child));
    return histogram;
}

std::vector<prometheus::MetricFamily> SingleWriterHistogramRegistry::Collect() const {
    const std::lock_guard<std::mutex> lock(registration_mutex_);

    std::vector<prometheus::MetricFamily> collected;
    collected.reserve(families_.size());
    for (const Family& family : families_) {
        prometheus::MetricFamily metric_family;
        metric_family.name = family.metric_name;
        metric_family.help = family.help;
        metric_family.type = prometheus::MetricType::Histogram;
        metric_family.metric.reserve(family.children.size());
        for (const Child& child : family.children) {
            prometheus::ClientMetric metric = child.histogram->collect();
            // In the order std::map holds them, which is by name: the order prometheus-cpp's
            // Family::CollectMetric adds them in, since its labels are a std::map too.
            metric.label.reserve(child.labels.size());
            for (const auto& [name, value] : child.labels) {
                prometheus::ClientMetric::Label label;
                label.name = name;
                label.value = value;
                metric.label.push_back(label);
            }
            metric_family.metric.push_back(std::move(metric));
        }
        collected.push_back(std::move(metric_family));
    }
    return collected;
}

} // namespaces
