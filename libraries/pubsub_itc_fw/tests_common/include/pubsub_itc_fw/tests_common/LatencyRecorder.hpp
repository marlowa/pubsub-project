#pragma once

#include <iostream>
#include <map>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>

namespace pubsub_itc_fw::tests {

/**
 * @brief Thread-safe latency recorder using nanosecond buckets.
 * Designed for NFT performance studies to identify long-tail outliers.
 */
class LatencyRecorder {
public:
    LatencyRecorder() = default;

    // Delete copy to avoid issues with atomic members
    LatencyRecorder(const LatencyRecorder&) = delete;
    LatencyRecorder& operator=(const LatencyRecorder&) = delete;

    /**
     * @brief Records a latency sample into the appropriate 10ns bucket.
     * @param duration_ns The measured duration in nanoseconds.
     */
    void record(int64_t duration_ns) {
        int64_t bucket_start = (duration_ns / 10) * 10;

        std::lock_guard<std::mutex> lock(buckets_mutex_);

        auto it = buckets_.find(bucket_start);
        if (it != buckets_.end()) {
            it->second.fetch_add(1, std::memory_order_relaxed);
        } else {
            buckets_[bucket_start].fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Dumps the data in a space-delimited format for scannable processing.
     * Format: bucket_start_ns count
     * @param label The dataset identifier (e.g., ALLOCATION).
     */
    void dump_space_delimited(const std::string& label) const {
        std::lock_guard<std::mutex> lock(buckets_mutex_);

        std::cout << "\n# DATASET: " << label << "\n";
        std::cout << "# ns_bucket_start count\n";

        // Collect and sort buckets for chronological output
        std::vector<std::pair<int64_t, size_t>> sorted_buckets;
        for (const auto& [bucket, count] : buckets_) {
            size_t val = count.load(std::memory_order_relaxed);
            if (val > 0) {
                sorted_buckets.push_back({bucket, val});
            }
        }

        std::sort(sorted_buckets.begin(), sorted_buckets.end());

        for (const auto& [bucket, count] : sorted_buckets) {
            std::cout << bucket << " " << count << "\n";
        }
        std::cout << "# END DATASET\n" << std::endl;
    }

    /**
     * @brief Human-readable dump for quick console inspection.
     */
    void dump_results(const std::string& label) const {
        std::lock_guard<std::mutex> lock(buckets_mutex_);

        std::cout << "\n--- Latency Histogram: " << label << " ---\n";
        std::cout << std::left << std::setw(15) << "Bucket (ns)" << " | Count\n";
        std::cout << "------------------\n";

        std::vector<std::pair<int64_t, size_t>> sorted_buckets;
        for (const auto& [bucket, count] : buckets_) {
            sorted_buckets.push_back({bucket, count.load()});
        }
        std::sort(sorted_buckets.begin(), sorted_buckets.end());

        for (const auto& [bucket, count] : sorted_buckets) {
            if (count > 0) {
                std::cout << std::right << std::setw(4) << bucket << "-"
                          << std::left << std::setw(4) << (bucket + 10)
                          << " | " << count << "\n";
            }
        }
    }

private:
    // map bucket_start -> atomic_counter
    mutable std::map<int64_t, std::atomic<size_t>> buckets_;
    mutable std::mutex buckets_mutex_;
};

} // namespace pubsub_itc_fw::tests
