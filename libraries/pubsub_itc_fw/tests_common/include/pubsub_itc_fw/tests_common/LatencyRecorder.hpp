#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <map>

class LatencyRecorder {
public:
    void record(uint64_t nanoseconds) {
        // Simple bucketing: 0-10ns, 10-20ns, ..., 1000+ns
        uint64_t bucket = nanoseconds / 10;
        if (bucket > 100) bucket = 101; // Catch-all for slow calls
        buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    void dump_results(const std::string& label) {
        std::cout << "\n--- Latency Histogram: " << label << " ---\n";
        std::cout << "Range (ns) | Count\n";
        std::cout << "------------------\n";
        for (int i = 0; i <= 101; ++i) {
            uint64_t count = buckets_[i].load();
            if (count > 0) {
                if (i <= 100)
                    std::cout << std::setw(4) << i*10 << "-" << (i+1)*10 << " | " << count << "\n";
                else
                    std::cout << "   1000+  | " << count << "\n";
            }
        }
    }

private:
    std::atomic<uint64_t> buckets_[102]{};
};
