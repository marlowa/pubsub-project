#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <prometheus/collectable.h>
#include <prometheus/metric_family.h>

#include <pubsub_itc_fw/SingleWriterHistogram.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Owns this process's histograms and hands them to the scrape, grouped into families
 *        exactly as the prometheus-cpp registry groups its own histograms.
 *
 * Design: docs/framework/single_writer_histogram.md, section 4.4.
 *
 * PrometheusEndpoint owns one of these and gives it to the prometheus::Exposer as a second
 * collectable, next to the registry that still holds the counters and gauges. A family is one
 * metric name with its help text; each registered key is a child of its family, carrying its
 * own labels and its own bucket bounds.
 *
 * Families are returned in the order their first child was registered, and children in the
 * order they were registered. The text a scrape returns for each family is produced by
 * prometheus-cpp's own serialiser, from the same structures prometheus::Histogram produces, so
 * it is the same text byte for byte.
 *
 * **Locking.** No thread that records values ever takes a lock here: recording goes straight to
 * a SingleWriterHistogram. The one mutex involved belongs to PrometheusEndpoint and is passed in
 * at construction. The endpoint holds it while it registers, which is while components start,
 * and Collect() takes it so that a registration cannot add to the families while a scrape walks
 * them. Borrowing the endpoint's mutex, rather than having one of its own, keeps it to one lock
 * that only registration and scrapes ever take.
 */
class SingleWriterHistogramRegistry : public prometheus::Collectable {
  public:
    ~SingleWriterHistogramRegistry() override = default;

    /**
     * @param[in] registration_mutex PrometheusEndpoint's mutex. Must outlive this object.
     */
    explicit SingleWriterHistogramRegistry(std::mutex& registration_mutex) : registration_mutex_(registration_mutex) {}

    SingleWriterHistogramRegistry(const SingleWriterHistogramRegistry&) = delete;
    SingleWriterHistogramRegistry& operator=(const SingleWriterHistogramRegistry&) = delete;
    SingleWriterHistogramRegistry(SingleWriterHistogramRegistry&&) = delete;
    SingleWriterHistogramRegistry& operator=(SingleWriterHistogramRegistry&&) = delete;

    /**
     * @brief Creates a histogram as a child of the family named, creating the family if this is
     *        its first child, and returns the histogram to record into.
     *
     * The caller must hold the registration mutex. PrometheusEndpoint has already checked that
     * the key is new and that the help text agrees with the family's, so neither is checked again.
     * The histogram lives as long as this object.
     *
     * @param[in] metric_name  The family's name, which is the metric name Prometheus shows.
     * @param[in] help         The family's help text. Used only when this creates the family.
     * @param[in] labels       This child's labels.
     * @param[in] upper_bounds This child's bucket upper bounds, strictly ascending.
     */
    SingleWriterHistogram& add(const std::string& metric_name, const std::string& help, const std::map<std::string, std::string>& labels,
                               const std::vector<double>& upper_bounds);

    /**
     * @brief Reads every histogram for a scrape. Called by the Exposer's threads, and by
     *        PrometheusEndpoint::exposition_text(). Takes the registration mutex.
     */
    std::vector<prometheus::MetricFamily> Collect() const override;

  private:
    struct Child {
        std::map<std::string, std::string> labels;
        // Held by pointer so that its address never changes as the vector of children grows:
        // the recording handles keep that address for the life of the process.
        std::unique_ptr<SingleWriterHistogram> histogram;
    };

    struct Family {
        std::string metric_name;
        std::string help;
        std::vector<Child> children;
    };

    std::mutex& registration_mutex_;

    std::vector<Family> families_;
    std::map<std::string, size_t> family_index_by_name_;
};

} // namespaces
