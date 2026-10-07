// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <prometheus/client_metric.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/SingleWriterHistogram.hpp>
#include <pubsub_itc_fw/SingleWriterHistogramRegistry.hpp>

using pubsub_itc_fw::PreconditionAssertion;
using pubsub_itc_fw::SingleWriterHistogram;
using pubsub_itc_fw::SingleWriterHistogramRegistry;

namespace {

using Labels = std::map<std::string, std::string>;

std::vector<int64_t> cumulative_counts(const prometheus::ClientMetric& metric) {
    std::vector<int64_t> counts;
    for (const prometheus::ClientMetric::Bucket& bucket : metric.histogram.bucket) {
        counts.push_back(static_cast<int64_t>(bucket.cumulative_count));
    }
    return counts;
}

std::string serialise(const std::vector<prometheus::MetricFamily>& families) {
    std::ostringstream stream;
    const prometheus::TextSerializer serializer;
    serializer.Serialize(stream, families);
    return stream.str();
}

std::vector<std::string> sorted_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// Values on each bound, between bounds, below the first and above the last, so every bucket
// and both edges of every bucket are exercised.
const std::vector<double> sample_values{0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 100.0, 1.0, 0.0};

} // un-named namespace

TEST(SingleWriterHistogramTest, AValueEqualToABoundIsCountedInThatBoundsBucket) {
    SingleWriterHistogram histogram({1.0, 2.0, 3.0});

    for (const double value : {0.5, 1.0, 1.5, 2.0, 3.0, 3.5}) {
        histogram.observe(value);
    }

    const prometheus::ClientMetric metric = histogram.collect();
    // At or below 1: 0.5 and 1.0. At or below 2: those and 1.5, 2.0. At or below 3: and 3.0.
    // Infinity: all six.
    EXPECT_EQ(cumulative_counts(metric), (std::vector<int64_t>{2, 4, 5, 6}));
    EXPECT_EQ(metric.histogram.sample_count, 6U);
    EXPECT_DOUBLE_EQ(metric.histogram.sample_sum, 11.5);
}

TEST(SingleWriterHistogramTest, TheFinalBucketsBoundIsInfinity) {
    SingleWriterHistogram histogram({10.0});
    histogram.observe(1e300);

    const prometheus::ClientMetric metric = histogram.collect();
    ASSERT_EQ(metric.histogram.bucket.size(), 2U);
    EXPECT_EQ(metric.histogram.bucket[0].cumulative_count, 0U);
    EXPECT_EQ(metric.histogram.bucket[1].upper_bound, std::numeric_limits<double>::infinity());
    EXPECT_EQ(metric.histogram.bucket[1].cumulative_count, 1U);
}

TEST(SingleWriterHistogramTest, AnEmptyHistogramReportsZeroEverywhere) {
    SingleWriterHistogram histogram({1.0, 2.0});

    const prometheus::ClientMetric metric = histogram.collect();
    EXPECT_EQ(cumulative_counts(metric), (std::vector<int64_t>{0, 0, 0}));
    EXPECT_EQ(metric.histogram.sample_count, 0U);
    EXPECT_EQ(metric.histogram.sample_sum, 0.0);
}

// More counts than fit in one cache line, so the count for a bucket has to be found in the
// right line as well as at the right place within it.
TEST(SingleWriterHistogramTest, CountsSpanningSeveralCacheLinesAreKeptApart) {
    std::vector<double> bounds;
    for (int bound = 1; bound <= 20; ++bound) {
        bounds.push_back(static_cast<double>(bound));
    }
    SingleWriterHistogram histogram(bounds);

    // Value v lands in bucket v - 1, so bucket i is given i + 1 values.
    for (int bucket = 0; bucket < 20; ++bucket) {
        for (int repeat = 0; repeat <= bucket; ++repeat) {
            histogram.observe(static_cast<double>(bucket + 1));
        }
    }

    const std::vector<int64_t> counts = cumulative_counts(histogram.collect());
    int64_t expected = 0;
    for (int bucket = 0; bucket < 20; ++bucket) {
        expected += bucket + 1;
        EXPECT_EQ(counts[static_cast<size_t>(bucket)], expected) << "bucket " << bucket;
    }
    EXPECT_EQ(counts.back(), expected);
}

TEST(SingleWriterHistogramTest, BoundsThatAreNotStrictlyAscendingAreRejected) {
    EXPECT_THROW(SingleWriterHistogram({2.0, 1.0}), PreconditionAssertion);
    EXPECT_THROW(SingleWriterHistogram({1.0, 1.0}), PreconditionAssertion);
}

// The scrape's text must not change: Prometheus, the dashboards and the reporting script all
// read it. The same values go into a prometheus::Histogram and into a SingleWriterHistogram
// under the same name, help text, labels and bounds, and the two serialised texts are compared.
TEST(SingleWriterHistogramTest, AScrapeRendersExactlyAsPrometheusCppDoes) {
    const std::vector<double> bounds{1.0, 2.0, 3.0};
    const Labels labels{{"application", "pubsub"}, {"component", "gateway"}, {"scope", "fix"}};

    prometheus::Registry reference_registry;
    auto& reference_family = prometheus::BuildHistogram().Name("latency_nanoseconds").Help("Latency").Register(reference_registry);
    prometheus::Histogram& reference = reference_family.Add(labels, bounds);

    std::mutex registration_mutex;
    SingleWriterHistogramRegistry registry(registration_mutex);
    SingleWriterHistogram& histogram = registry.add("latency_nanoseconds", "Latency", labels, bounds);

    for (const double value : sample_values) {
        reference.Observe(value);
        histogram.observe(value);
    }

    EXPECT_EQ(serialise(registry.Collect()), serialise(reference_registry.Collect()));
}

// Two scopes of one metric with different bounds, which is a family with two children.
// prometheus-cpp keeps a family's children in an unordered_map, so the order it renders them in
// is not defined; the lines are compared as sorted sets for that reason.
TEST(SingleWriterHistogramTest, TwoScopesWithDifferentBoundsRenderAsPrometheusCppDoes) {
    const Labels fix_labels{{"application", "pubsub"}, {"component", "gateway"}, {"scope", "fix"}};
    const Labels binary_labels{{"application", "pubsub"}, {"component", "gateway"}, {"scope", "binary"}};
    const std::vector<double> fix_bounds{1.0, 3.0};
    const std::vector<double> binary_bounds{0.5, 2.0, 2.5, 10.0};

    prometheus::Registry reference_registry;
    auto& reference_family = prometheus::BuildHistogram().Name("latency_nanoseconds").Help("Latency").Register(reference_registry);
    prometheus::Histogram& reference_fix = reference_family.Add(fix_labels, fix_bounds);
    prometheus::Histogram& reference_binary = reference_family.Add(binary_labels, binary_bounds);

    std::mutex registration_mutex;
    SingleWriterHistogramRegistry registry(registration_mutex);
    SingleWriterHistogram& fix = registry.add("latency_nanoseconds", "Latency", fix_labels, fix_bounds);
    SingleWriterHistogram& binary = registry.add("latency_nanoseconds", "Latency", binary_labels, binary_bounds);

    for (const double value : sample_values) {
        reference_fix.Observe(value);
        fix.observe(value);
        reference_binary.Observe(value * 2.0);
        binary.observe(value * 2.0);
    }

    const std::string ours = serialise(registry.Collect());
    EXPECT_EQ(sorted_lines(ours), sorted_lines(serialise(reference_registry.Collect()))) << ours;
}

TEST(SingleWriterHistogramTest, FamiliesComeOutInTheOrderTheyWereFirstRegistered) {
    std::mutex registration_mutex;
    SingleWriterHistogramRegistry registry(registration_mutex);
    registry.add("zebra_nanoseconds", "Zebra", Labels{{"scope", "a"}}, {1.0});
    registry.add("aardvark_nanoseconds", "Aardvark", Labels{{"scope", "a"}}, {1.0});
    registry.add("zebra_nanoseconds", "Zebra", Labels{{"scope", "b"}}, {1.0});

    const std::vector<prometheus::MetricFamily> families = registry.Collect();
    ASSERT_EQ(families.size(), 2U);
    EXPECT_EQ(families[0].name, "zebra_nanoseconds");
    EXPECT_EQ(families[0].metric.size(), 2U);
    EXPECT_EQ(families[1].name, "aardvark_nanoseconds");
}

// One thread records a known number of values while another collects continuously. No bucket's
// count may ever be seen to fall, and once the writer has finished, the total and the sum must
// be exact: a lost increment would show as a total below the number recorded.
TEST(SingleWriterHistogramTest, AScrapeRunningAlongsideTheWriterSeesNoCountFallAndLosesNothing) {
    const std::vector<double> bounds{10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0};
    SingleWriterHistogram histogram(bounds);
    constexpr int64_t values_to_record = 2'000'000;

    std::atomic<bool> writer_finished{false};
    std::thread writer([&histogram, &writer_finished]() {
        for (int64_t index = 0; index < values_to_record; ++index) {
            histogram.observe(static_cast<double>(index % 100));
        }
        writer_finished.store(true, std::memory_order_release);
    });

    std::vector<int64_t> previous_per_bucket(bounds.size() + 1, 0);
    int64_t collects = 0;
    bool a_count_fell = false;
    while (!writer_finished.load(std::memory_order_acquire)) {
        const std::vector<int64_t> cumulative = cumulative_counts(histogram.collect());
        int64_t below = 0;
        for (size_t bucket = 0; bucket < cumulative.size(); ++bucket) {
            const int64_t in_this_bucket = cumulative[bucket] - below;
            below = cumulative[bucket];
            if (in_this_bucket < previous_per_bucket[bucket]) {
                a_count_fell = true;
            }
            previous_per_bucket[bucket] = in_this_bucket;
        }
        ++collects;
    }
    writer.join();

    EXPECT_FALSE(a_count_fell);
    EXPECT_GT(collects, 0) << "the reader never ran alongside the writer, so this proved nothing";

    const prometheus::ClientMetric final_metric = histogram.collect();
    EXPECT_EQ(static_cast<int64_t>(final_metric.histogram.sample_count), values_to_record);
    // 0 to 99 sums to 4950, and each appears values_to_record / 100 times. Every partial sum is a
    // whole number well inside the range a double holds exactly, so the sum must be exact too.
    EXPECT_EQ(final_metric.histogram.sample_sum, 4950.0 * static_cast<double>(values_to_record / 100));
}

#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
TEST(SingleWriterHistogramTest, ASecondThreadRecordingIsRejected) {
    SingleWriterHistogram histogram({1.0});
    histogram.observe(0.5);

    bool threw_precondition = false;
    std::thread intruder([&histogram, &threw_precondition]() {
        try {
            histogram.observe(0.5);
        } catch (const PreconditionAssertion&) {
            threw_precondition = true;
        }
    });
    intruder.join();
    EXPECT_TRUE(threw_precondition) << "a second thread was allowed to record into a single-writer histogram";
}

TEST(SingleWriterHistogramTest, TheFirstThreadToRecordOwnsTheHistogram) {
    SingleWriterHistogram histogram({1.0});
    std::thread owner([&histogram]() {
        histogram.observe(0.5);
        histogram.observe(0.5);
    });
    owner.join();
    EXPECT_EQ(histogram.collect().histogram.sample_count, 2U);
}
#endif
