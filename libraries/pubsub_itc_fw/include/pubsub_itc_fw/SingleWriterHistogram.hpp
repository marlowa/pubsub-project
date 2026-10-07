#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

// The thread check needs these two only in the builds that compile it in. They are guarded
// separately so that each still sits in the section the include-order rule puts it in.
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <thread>
#endif

#include <prometheus/client_metric.h>

#include <pubsub_itc_fw/HistogramInterface.hpp>
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#endif

namespace pubsub_itc_fw {

/**
 * @brief A histogram that one thread records into without a lock, and that the scrape reads
 *        without stopping that thread.
 *
 * Design and measurements: docs/framework/single_writer_histogram.md.
 *
 * prometheus::Histogram locks a mutex on every observation, and the scrape thread locks the
 * same mutex while it reads, so a thread recording a value during a scrape goes to sleep in the
 * kernel until the scrape lets go. This class is recorded into by exactly one thread. Because
 * no other thread ever writes it, an increment can be a read followed by a write, with no lock
 * and no atomic read-modify-write instruction: there is no second writer whose update could be
 * lost.
 *
 * The counts and the sum are std::atomic only for the sake of the scrape thread that reads
 * them. A relaxed atomic load or store of a 64-bit value compiles to an ordinary move on x86-64,
 * so the recording thread pays nothing for it; what it buys is that the scrape's reads are
 * defined behaviour in C++, and can never see half of an old value and half of a new one.
 *
 * **What a scrape sees.** collect() reads the counts one at a time while the writer may still be
 * recording, so they are not one instantaneous picture. Every count read is a value that bucket
 * really had, no count ever goes down from one scrape to the next, and the cumulative counts and
 * the total are worked out from the one set of values read, so they are consistent with each
 * other. The sum may include or leave out a value recorded while the scrape was part way
 * through; the next scrape includes it.
 *
 * **One writer.** The whole design depends on it. In builds with PUBSUB_ITC_FW_THREAD_CHECKS
 * (Debug, AddressSanitizer and coverage), the first thread to call observe() owns the histogram
 * and any other thread that calls it raises PreconditionAssertion. Registration does not claim
 * ownership, because registering a histogram and recording into it need not happen on the same
 * thread.
 */
class SingleWriterHistogram : public HistogramInterface {
  public:
    ~SingleWriterHistogram() override = default;

    /**
     * @param[in] upper_bounds Bucket upper bounds, strictly ascending. A value equal to a bound
     *                         is counted in that bound's bucket, and a value above every bound in
     *                         a final bucket whose bound is infinity.
     *
     * Raises PreconditionAssertion if the bounds are not strictly ascending. The configuration
     * loaders check this when they read the bounds, so reaching it here is a programming error.
     */
    explicit SingleWriterHistogram(const std::vector<double>& upper_bounds);

    SingleWriterHistogram(const SingleWriterHistogram&) = delete;
    SingleWriterHistogram& operator=(const SingleWriterHistogram&) = delete;
    SingleWriterHistogram(SingleWriterHistogram&&) = delete;
    SingleWriterHistogram& operator=(SingleWriterHistogram&&) = delete;

    /**
     * @brief Records one value. Called by the one thread that owns this histogram.
     *
     * Defined here so that it can be inlined at the call site, as the whole cost of recording
     * is meant to be a search of the bounds and two stores.
     */
    void observe(double value) override {
        check_single_writer();

        // lower_bound finds the first bound not less than the value, so a value equal to a bound
        // lands in that bound's bucket: Prometheus defines a bucket as "less than or equal to".
        const size_t bucket_index =
            static_cast<size_t>(std::distance(upper_bounds_.begin(), std::lower_bound(upper_bounds_.begin(), upper_bounds_.end(), value)));

        std::atomic<int64_t>& count = count_for(bucket_index);
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        sum_.store(sum_.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
    }

    /**
     * @brief Reads the histogram for a scrape. Safe to call from any thread at any time,
     *        including while the owning thread records.
     *
     * Returns the same shape prometheus::Histogram::Collect returns: one bucket for each bound
     * and a final one whose bound is infinity, each holding the count of values at or below its
     * bound, plus the total and the sum. Labels are left for the caller to add.
     */
    [[nodiscard]] prometheus::ClientMetric collect() const;

  private:
    // Eight 64-bit counts fill one 64-byte cache line. Each histogram's counts are kept in
    // whole lines of their own, so that two histograms recorded by different threads never
    // share a line. If they did, each write by one thread would take the line away from the
    // other's core, and the other's next write would have to fetch it back.
    static constexpr size_t counts_per_cache_line = 8;

    struct alignas(64) CacheLineOfCounts {
        std::atomic<int64_t> counts[counts_per_cache_line]{};
    };

    std::atomic<int64_t>& count_for(size_t bucket_index) {
        return cache_lines_[bucket_index / counts_per_cache_line].counts[bucket_index % counts_per_cache_line];
    }

    [[nodiscard]] const std::atomic<int64_t>& count_for(size_t bucket_index) const {
        return cache_lines_[bucket_index / counts_per_cache_line].counts[bucket_index % counts_per_cache_line];
    }

    // Compiled to nothing unless PUBSUB_ITC_FW_THREAD_CHECKS is set, as in IncrementalRehashMap,
    // so the release build records with no check at all.
    void check_single_writer() {
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
        const std::thread::id calling_thread = std::this_thread::get_id();
        if (owning_thread_ == std::thread::id{}) {
            owning_thread_ = calling_thread;
            return;
        }
        if (owning_thread_ != calling_thread) {
            throw PreconditionAssertion("SingleWriterHistogram has one writer and a second thread recorded into it", __FILE__, __LINE__);
        }
#endif
    }

    // Fixed at construction and only read afterwards, by the owning thread and by the scrape.
    const std::vector<double> upper_bounds_;

    // One count for each bound, then one for values above every bound.
    std::vector<CacheLineOfCounts> cache_lines_;

#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
    std::thread::id owning_thread_{};
#endif

    // On a cache line of its own, away from the members above, which the scrape reads.
    alignas(64) std::atomic<double> sum_{0.0};
};

} // namespaces
