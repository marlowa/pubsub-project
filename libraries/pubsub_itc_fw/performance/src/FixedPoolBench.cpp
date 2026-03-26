/*
Here is some information on how to run this performance program using perf.

perf stat ./build/libraries/pubsub_itc_fw/performance/fixed_pool_bench


If you get the error below, the remedy follows:

Error:
Access to performance monitoring and observability operations is limited.
Consider adjusting /proc/sys/kernel/perf_event_paranoid setting to open
access to performance monitoring and observability operations for processes
without CAP_PERFMON, CAP_SYS_PTRACE or CAP_SYS_ADMIN Linux capability.
More information can be found at 'Perf events and tool security' document:
https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html
perf_event_paranoid setting is 4:
  -1: Allow use of (almost) all events by all users
      Ignore mlock limit after perf_event_mlock_kb without CAP_IPC_LOCK
>= 0: Disallow raw and ftrace function tracepoint access
>= 1: Disallow CPU event access
>= 2: Disallow kernel profiling
To make the adjusted perf_event_paranoid setting permanent preserve it
in /etc/sysctl.conf (e.g. kernel.perf_event_paranoid = <setting>)

Here is the remedy:

sudo sysctl -w kernel.kptr_restrict=0
sudo sysctl -w kernel.perf_event_paranoid=0
echo 'kernel.perf_event_paranoid=0' | sudo tee -a /etc/sysctl.conf

Below are the results I got:

fixed_pool_bench:
  produced: 5000000
  consumed: 5000000
  elapsed:  199559212 ns
  throughput: 2.50552e+07 ops/sec

 Performance counter stats for './build/libraries/pubsub_itc_fw/performance/fixed_pool_bench':

            400.27 msec task-clock                       #    1.985 CPUs utilized
                 6      context-switches                 #   14.990 /sec
                 4      cpu-migrations                   #    9.993 /sec
               171      page-faults                      #  427.217 /sec
       410,231,474      cpu_atom/instructions/           #    0.78  insn per cycle              (0.08%)
     1,508,273,262      cpu_core/instructions/           #    0.69  insn per cycle              (99.89%)
       527,565,839      cpu_atom/cycles/                 #    1.318 GHz                         (0.10%)
     2,172,259,375      cpu_core/cycles/                 #    5.427 GHz                         (99.89%)
        61,380,683      cpu_atom/branches/               #  153.350 M/sec                       (0.11%)
       299,820,115      cpu_core/branches/               #  749.054 M/sec                       (99.89%)
         3,819,428      cpu_atom/branch-misses/          #    6.22% of all branches             (0.11%)
         4,347,553      cpu_core/branch-misses/          #    1.45% of all branches             (99.89%)
             TopdownL1 (cpu_core)                 #      4.7 %  tma_backend_bound
                                                  #     24.2 %  tma_bad_speculation
                                                  #     40.1 %  tma_frontend_bound
                                                  #     31.0 %  tma_retiring             (99.89%)
                                                  #      8.8 %  tma_bad_speculation
                                                  #     16.4 %  tma_retiring             (0.11%)
                                                  #     31.4 %  tma_backend_bound
                                                  #     43.4 %  tma_frontend_bound       (0.11%)

       0.201651842 seconds time elapsed

       0.386278000 seconds user
       0.015010000 seconds sys

flamegraph (perl scripts) is useful to install, must be git cloned:

git clone https://github.com/brendangregg/FlameGraph

       The command below is used to get a flame graph.

perf record -g --call-graph dwarf -F 999 \
    ./build/libraries/pubsub_itc_fw/performance/fixed_pool_bench

perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg

install imagemagick to be able to convert svg files to jpg.

 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

using namespace pubsub_itc_fw;

// -----------------------------------------------------------------------------
// Simple test object
// -----------------------------------------------------------------------------
struct TestObject {
    int id_{0};
    std::byte padding_[64]; // keep it non-trivial, but not huge
};

// -----------------------------------------------------------------------------
// CPU pinning helper
// -----------------------------------------------------------------------------
static bool PinToCpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// -----------------------------------------------------------------------------
// Single-producer / single-consumer ring buffer of pointers
// -----------------------------------------------------------------------------
template <typename T>
class SpscRing {
  public:
    explicit SpscRing(std::size_t capacity)
        : capacity_(capacity),
          mask_(capacity - 1),
          buffer_(capacity) {
        // Require power-of-two capacity
        assert((capacity_ & mask_) == 0 && "SpscRing capacity must be power of two");
    }

    bool Push(T* ptr) {
        auto head = head_.load(std::memory_order_relaxed);
        auto next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > capacity_) {
            return false; // full
        }
        buffer_[head & mask_] = ptr;
        head_.store(next, std::memory_order_release);
        return true;
    }

    T* Pop() {
        auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return nullptr; // empty
        }
        T* ptr = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return ptr;
    }

  private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T*> buffer_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

// -----------------------------------------------------------------------------
// Optional huge-page error handler (no-op for this bench)
// -----------------------------------------------------------------------------
static std::function<void(void*, std::size_t)> MakeHugePageErrorHandler() {
    return [](void*, std::size_t) {
        // Intentionally no-op: we just want the production path,
        // and we don't care about logging here.
    };
}

// -----------------------------------------------------------------------------
// Main benchmark
// -----------------------------------------------------------------------------
int main() {
    // Tunables: adjust for your machine / perf session
    const int poolCapacity          = 1024;          // number of slots in pool
    const std::size_t ringCapacity  = 1024;          // must be power of two
    const std::size_t iterations    = 50'000'000;     // producer iterations
    const int producerCpu           = 2;             // adjust as needed
    const int consumerCpu           = 3;             // adjust as needed
    const bool simulateSlowConsumer = true;
    const int consumerBusyWorkIters = 50;            // small spin to lag consumer

    FixedSizeMemoryPool<TestObject> pool(
        poolCapacity,
        UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages),
        MakeHugePageErrorHandler()
    );

    SpscRing<TestObject> ring(ringCapacity);

    std::atomic<bool> startFlag{false};
    std::atomic<bool> doneFlag{false};
    std::atomic<std::size_t> produced{0};
    std::atomic<std::size_t> consumed{0};

    // Optional: CAS failure counter (for manual inspection)
    // If you want to wire this into the pool, you can add a hook there.
    // For now, we just focus on perf/flamegraphs.

    std::thread producer([&]() {
        if (!PinToCpu(producerCpu)) {
            std::cerr << "Producer: failed to pin to CPU " << producerCpu << "\n";
        }

        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (std::size_t i = 0; i < iterations; ++i) {
            // Allocate from pool (raw memory)
            TestObject* obj = nullptr;
            while ((obj = pool.allocate()) == nullptr) {
                // Pool exhausted: let consumer catch up
                std::this_thread::yield();
            }

            // Construct object in-place
            obj = new (obj) TestObject();
            obj->id_ = static_cast<int>(i);

            // Enqueue into ring
            while (!ring.Push(obj)) {
                std::this_thread::yield();
            }

            produced.fetch_add(1, std::memory_order_relaxed);
        }

        doneFlag.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        if (!PinToCpu(consumerCpu)) {
            std::cerr << "Consumer: failed to pin to CPU " << consumerCpu << "\n";
        }

        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (;;) {
            TestObject* obj = ring.Pop();
            if (obj) {
                // Simulate slightly slower consumer
                if (simulateSlowConsumer) {
                    for (int k = 0; k < consumerBusyWorkIters; ++k) {
                        asm volatile("" ::: "memory");
                    }
                }

                // Consume: trivial read
                const int id = obj->id_;
                (void)id;

                // Destroy and return to pool
                obj->~TestObject();
                pool.deallocate(obj);

                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (doneFlag.load(std::memory_order_acquire) &&
                    consumed.load(std::memory_order_relaxed) >= produced.load(std::memory_order_relaxed)) {
                    break;
                }
                std::this_thread::yield();
            }
        }
    });

    startFlag.store(true, std::memory_order_release);

    auto t0 = std::chrono::steady_clock::now();
    producer.join();
    consumer.join();
    auto t1 = std::chrono::steady_clock::now();

    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    std::cout << "fixed_pool_bench:\n";
    std::cout << "  produced: " << produced.load() << "\n";
    std::cout << "  consumed: " << consumed.load() << "\n";
    std::cout << "  elapsed:  " << elapsedNs << " ns\n";
    if (elapsedNs > 0) {
        const double opsPerSec = static_cast<double>(consumed.load()) * 1e9 / static_cast<double>(elapsedNs);
        std::cout << "  throughput: " << opsPerSec << " ops/sec\n";
    }

    if (consumed.load() != iterations) {
        std::cerr << "ERROR: consumed != iterations\n";
        return 1;
    }

    return 0;
}
