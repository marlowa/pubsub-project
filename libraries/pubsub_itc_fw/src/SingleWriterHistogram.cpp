// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include <prometheus/client_metric.h>

#include <pubsub_itc_fw/SingleWriterHistogram.hpp>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

namespace {

bool strictly_ascending(const std::vector<double>& values) {
    for (size_t index = 1; index < values.size(); ++index) {
        if (!(values[index - 1] < values[index])) {
            return false;
        }
    }
    return true;
}

// Rounded up, so the final, partly used cache line still belongs to this histogram alone.
size_t cache_lines_needed(size_t count_total, size_t counts_per_line) {
    return (count_total + counts_per_line - 1) / counts_per_line;
}

} // un-named namespace

SingleWriterHistogram::SingleWriterHistogram(const std::vector<double>& upper_bounds)
    : upper_bounds_(upper_bounds), cache_lines_(cache_lines_needed(upper_bounds.size() + 1, counts_per_cache_line)) {
    if (!strictly_ascending(upper_bounds_)) {
        throw PreconditionAssertion("SingleWriterHistogram: bucket upper bounds must be strictly ascending", __FILE__, __LINE__);
    }
}

prometheus::ClientMetric SingleWriterHistogram::collect() const {
    prometheus::ClientMetric metric;
    metric.histogram.bucket.reserve(upper_bounds_.size() + 1);

    // Each count is read once, and every figure reported is worked out from those reads, so the
    // cumulative counts never fall from one bucket to the next and the total always equals the
    // final bucket's count, however the writer's stores interleave with these loads.
    int64_t cumulative_count = 0;
    for (size_t index = 0; index <= upper_bounds_.size(); ++index) {
        cumulative_count += count_for(index).load(std::memory_order_relaxed);
        prometheus::ClientMetric::Bucket bucket;
        bucket.cumulative_count = static_cast<uint64_t>(cumulative_count);
        bucket.upper_bound = index == upper_bounds_.size() ? std::numeric_limits<double>::infinity() : upper_bounds_[index];
        metric.histogram.bucket.push_back(bucket);
    }
    metric.histogram.sample_count = static_cast<uint64_t>(cumulative_count);
    metric.histogram.sample_sum = sum_.load(std::memory_order_relaxed);
    return metric;
}

} // namespaces
