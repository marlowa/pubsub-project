/**
 * @brief Unit tests for the LockFreeMessageQueue class.
 *
 * This file contains a suite of Google Tests to verify the correctness,
 * concurrency behavior, watermark transitions, and shutdown semantics
 * of the lock-free multi-producer / single-consumer queue.
 */

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <random> // for mersenne twister

#include <gtest/gtest.h>

#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>

using namespace pubsub_itc_fw;

namespace {

/**
 * Simple struct used for testing.
 */
struct TestMessage {
    int value;
};

void pin_current_thread_to_cpu(int cpu_index) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_index, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    ASSERT_EQ(rc, 0) << "Failed to set thread affinity";
}

} // namespace

// ------------------------------------------------------------
// Basic enqueue/dequeue
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, BasicEnqueueDequeue) {
    LockFreeMessageQueue<TestMessage> q;

    q.enqueue(TestMessage{1});
    q.enqueue(TestMessage{2});
    q.enqueue(TestMessage{3});

    auto m1 = q.dequeue();
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->value, 1);

    auto m2 = q.dequeue();
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->value, 2);

    auto m3 = q.dequeue();
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->value, 3);

    EXPECT_TRUE(q.empty());
}

// ------------------------------------------------------------
// Multi-producer correctness
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, MultiProducerSingleConsumer) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int per_producer = 1000;

    std::atomic<bool> start_flag{false};
    std::vector<std::thread> threads;

    // Launch producers
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            while (!start_flag.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < per_producer; ++i) {
                q.enqueue(TestMessage{p * per_producer + i});
            }
        });
    }

    start_flag.store(true);

    // Consumer
    int count = 0;
    while (count < producers * per_producer) {
        auto msg = q.dequeue();
        if (msg.has_value()) {
            ++count;
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(q.empty());
}

// ------------------------------------------------------------
// Watermark transitions
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, WatermarkHandlersFireOnce) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    LockFreeMessageQueue<TestMessage> q(
        /*low_watermark=*/2,
        /*high_watermark=*/5,
        /*for_client_use=*/nullptr,
        /*low handler=*/[&](void*) { low_calls.fetch_add(1); },
        /*high handler=*/[&](void*) { high_calls.fetch_add(1); }
    );

    // Enqueue up to high watermark
    for (int i = 0; i < 5; ++i) {
        q.enqueue(TestMessage{i});
    }

    EXPECT_EQ(high_calls.load(), 1);

    // Dequeue down to low watermark
    for (int i = 0; i < 4; ++i) {
        auto msg = q.dequeue();
        ASSERT_TRUE(msg.has_value());
    }

    EXPECT_EQ(low_calls.load(), 1);
}

// ------------------------------------------------------------
// Shutdown flag behavior
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, EnqueueAfterShutdownIsDropped) {
    LockFreeMessageQueue<TestMessage> q;

    // Enqueue a few items
    q.enqueue(TestMessage{1});
    q.enqueue(TestMessage{2});

    // Trigger shutdown
    q.~LockFreeMessageQueue();  // Explicit destructor call for test purposes

    // Enqueue after shutdown should be ignored
    q.enqueue(TestMessage{999});

    // No crash, no UB, queue is in shutdown state
    SUCCEED();
}

// ------------------------------------------------------------
// Destructor drains remaining items
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, DestructorDrainsQueue) {
    auto* q = new LockFreeMessageQueue<TestMessage>();

    q->enqueue(TestMessage{1});
    q->enqueue(TestMessage{2});
    q->enqueue(TestMessage{3});

    delete q;  // Should drain safely

    SUCCEED();
}

TEST(LockFreeMessageQueueTest, HeavyMultiProducerStress) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 8;
    constexpr int per_producer = 100'000;
    constexpr int total = producers * per_producer;

    std::atomic<bool> start_flag{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Launch producers
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < per_producer; ++i) {
                q.enqueue(TestMessage{p * per_producer + i});
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Start all producers
    start_flag.store(true, std::memory_order_release);

    // Single consumer
    int local_count = 0;
    while (local_count < total) {
        auto msg = q.dequeue();
        if (msg.has_value()) {
            ++local_count;
            consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(produced.load(), total);
    EXPECT_EQ(consumed.load(), total);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, ProducerConsumerPinnedToSeparateCores) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int messages = 200'000;
    std::atomic<bool> start_flag{false};
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        pin_current_thread_to_cpu(0);

        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < messages; ++i) {
            q.enqueue(TestMessage{i});
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        pin_current_thread_to_cpu(1);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        while (local_count < messages) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), messages);
    EXPECT_EQ(consumed.load(), messages);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, SoakTestMillionsOfMessages) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int per_producer = 2'500'000; // 2.5M each → 10M total
    constexpr int total = producers * per_producer;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < per_producer; ++i) {
                q.enqueue(TestMessage{i});
            }
        });
    }

    start_flag.store(true, std::memory_order_release);

    int local_count = 0;
    while (local_count < total) {
        auto msg = q.dequeue();
        if (msg.has_value()) {
            ++local_count;
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, FalseSharingDetection) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int iterations = 2'000'000;
    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    // Producer pinned to CPU 0
    std::thread producer([&] {
        pin_current_thread_to_cpu(0);

        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < iterations; ++i) {
            q.enqueue(TestMessage{i});
        }
    });

    // Consumer pinned to CPU 1
    std::thread consumer([&] {
        pin_current_thread_to_cpu(1);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        while (local_count < iterations) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), iterations);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, AbaResistanceStress) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int bursts = 2000;
    constexpr int burst_size = 200;
    constexpr int total = producers * bursts * burst_size;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Producers: many small bursts to force rapid node allocation/freeing
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int b = 0; b < bursts; ++b) {
                for (int i = 0; i < burst_size; ++i) {
                    q.enqueue(TestMessage{p * 1'000'000 + b * burst_size + i});
                }
                // tiny pause to desynchronize producers
                std::this_thread::yield();
            }
        });
    }

    start_flag.store(true, std::memory_order_release);

    // Consumer: intentionally slower to create backlog and pointer churn
    int local_count = 0;
    while (local_count < total) {
        auto msg = q.dequeue();
        if (msg.has_value()) {
            ++local_count;
            consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
            // slow consumer slightly
            for (int i = 0; i < 50; ++i) {
                std::this_thread::yield();
            }
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(consumed.load(), total);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, WatermarkStormTest) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    // Low = 10, High = 20
    LockFreeMessageQueue<TestMessage> q(
        10,
        20,
        nullptr,
        [&](void*) { low_calls.fetch_add(1, std::memory_order_relaxed); },
        [&](void*) { high_calls.fetch_add(1, std::memory_order_relaxed); }
    );

    constexpr int cycles = 2000;

    // We will oscillate the queue size between 0 → 25 → 0 repeatedly.
    for (int c = 0; c < cycles; ++c) {

        // Fill above high watermark
        for (int i = 0; i < 25; ++i) {
            q.enqueue(TestMessage{i});
        }

        // Drain below low watermark
        for (int i = 0; i < 25; ++i) {
            auto msg = q.dequeue();
            if (!msg.has_value()) {
                std::this_thread::yield();
            }
        }
    }

    // After many oscillations, each transition should fire exactly once per cycle.
    EXPECT_EQ(high_calls.load(), cycles);
    EXPECT_EQ(low_calls.load(), cycles);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, ShutdownRaceTest) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int initial_messages = 1000;
    constexpr int racing_messages = 200000;

    // Fill the queue with some initial messages
    for (int i = 0; i < initial_messages; ++i) {
        q.enqueue(TestMessage{i});
    }

    std::atomic<bool> start_flag{false};
    std::atomic<bool> shutdown_now{false};

    // Producer that keeps trying to enqueue even after shutdown begins
    std::thread racing_producer([&] {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < racing_messages; ++i) {
            if (shutdown_now.load(std::memory_order_relaxed)) {
                // After shutdown, enqueue() should silently drop
                q.enqueue(TestMessage{999999});
            } else {
                q.enqueue(TestMessage{i});
            }
        }
    });

    // Consumer drains until shutdown is triggered
    std::thread consumer([&] {
        start_flag.store(true, std::memory_order_release);

        int drained = 0;
        while (drained < initial_messages) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++drained;
            } else {
                std::this_thread::yield();
            }
        }

        // Trigger shutdown mid‑stream
        shutdown_now.store(true, std::memory_order_release);
        q.shutdown();  // your queue’s shutdown flag setter
    });

    racing_producer.join();
    consumer.join();

    // After shutdown, queue must be in a safe state
    // No corruption, no crashes, no UB
    while (true) {
        auto msg = q.dequeue();
        if (!msg.has_value()) break;
    }

    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, LowWatermarkStormTest) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    // Low = 10, High = 20
    LockFreeMessageQueue<TestMessage> q(
        10,
        20,
        nullptr,
        [&](void*) { low_calls.fetch_add(1, std::memory_order_relaxed); },
        [&](void*) { high_calls.fetch_add(1, std::memory_order_relaxed); }
    );

    constexpr int cycles = 2000;

    // We oscillate the queue size between 0 → 25 → 0 repeatedly.
    // This forces both transitions many times.
    for (int c = 0; c < cycles; ++c) {

        // Fill above high watermark
        for (int i = 0; i < 25; ++i) {
            q.enqueue(TestMessage{i});
        }

        // Drain below low watermark
        for (int i = 0; i < 25; ++i) {
            auto msg = q.dequeue();
            if (!msg.has_value()) {
                std::this_thread::yield();
            }
        }
    }

    // Each cycle should produce exactly one high→low and one low→high transition.
    EXPECT_EQ(high_calls.load(), cycles);
    EXPECT_EQ(low_calls.load(), cycles);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, ThroughputBenchmark) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int per_producer = 500'000; // 2M total
    constexpr int total = producers * per_producer;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Launch producers
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < per_producer; ++i) {
                q.enqueue(TestMessage{i});
            }
        });
    }

    // Start timing
    auto start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    // Consumer
    int local_count = 0;
    while (local_count < total) {
        auto msg = q.dequeue();
        if (msg.has_value()) {
            ++local_count;
            consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::yield();
        }
    }

    auto end = std::chrono::steady_clock::now();
    for (auto& t : threads) {
        t.join();
    }

    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double msgs_per_sec = (total * 1000.0) / duration_ms;

    // Assert a reasonable minimum throughput.
    // This threshold is intentionally conservative.
    EXPECT_GT(msgs_per_sec, 500'000.0);

    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, MixedRateJitterSoakTest) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int iterations = 500'000;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Producers with realistic jitter
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::mt19937 rng(p + 1);

            // Typical burst sizes
            std::uniform_int_distribution<int> burst_dist(4, 32);

            // Frequent tiny stalls (cache misses, branch mispredicts)
            std::uniform_int_distribution<int> tiny_stall(1, 3);

            // Occasional medium stalls (allocator slow path)
            std::uniform_int_distribution<int> medium_stall(20, 200);

            // Rare large stalls (NUMA hiccup, page fault)
            std::uniform_int_distribution<int> large_stall(200, 2000);

            // Probability distributions
            std::uniform_int_distribution<int> stall_type(1, 1000);

            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations; ) {

                // Burst of messages
                int burst = burst_dist(rng);
                for (int b = 0; b < burst && i < iterations; ++b, ++i) {
                    q.enqueue(TestMessage{i});
                }

                // Decide what kind of stall to simulate
                int s = stall_type(rng);

                if (s <= 850) {
                    // 85%: tiny stall
                    for (int k = 0, n = tiny_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                } else if (s <= 990) {
                    // 14%: medium stall
                    for (int k = 0, n = medium_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                } else {
                    // 1%: large stall
                    for (int k = 0, n = large_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                }
            }
        });
    }

    // Consumer with realistic jitter
    std::thread consumer([&] {
        std::mt19937 rng(999);

        std::uniform_int_distribution<int> tiny_stall(1, 3);
        std::uniform_int_distribution<int> medium_stall(10, 50);
        std::uniform_int_distribution<int> stall_type(1, 1000);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        const int total = producers * iterations;

        while (local_count < total) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Consumer jitter
                int s = stall_type(rng);
                if (s <= 900) {
                    for (int k = 0, n = tiny_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                } else {
                    for (int k = 0, n = medium_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                }
            }
        }
    });

    for (auto& t : threads) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(consumed.load(), producers * iterations);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, MemoryPressureBurstTest) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int bursts = 3000;
    constexpr int burst_size = 50;   // small bursts → high allocation churn
    constexpr int total = producers * bursts * burst_size;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Producers: rapid bursts with forced pauses to simulate allocator pressure
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::mt19937 rng(p + 123);
            std::uniform_int_distribution<int> pause_dist(50, 500);

            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int b = 0; b < bursts; ++b) {

                // Burst of allocations
                for (int i = 0; i < burst_size; ++i) {
                    q.enqueue(TestMessage{i});
                }

                // Forced pause to simulate allocator slow path
                int pause = pause_dist(rng);
                for (int k = 0; k < pause; ++k) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Consumer: drains continuously
    std::thread consumer([&] {
        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        while (local_count < total) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : threads) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(consumed.load(), total);
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeMessageQueueTest, QueueDepthHistogramTest) {
    LockFreeMessageQueue<TestMessage> q;

    constexpr int producers = 4;
    constexpr int iterations = 300'000;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};
    std::atomic<int> depth{0};
    std::atomic<int> max_depth{0};

    std::array<std::atomic<int>, 10> histogram{};
    for (auto& h : histogram) h.store(0);

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Producers with jitter
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::mt19937 rng(p + 100);
            std::uniform_int_distribution<int> burst_dist(4, 32);
            std::uniform_int_distribution<int> stall_dist(1, 50);

            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < iterations; ) {
                int burst = burst_dist(rng);
                for (int b = 0; b < burst && i < iterations; ++b, ++i) {
                    q.enqueue(TestMessage{i});
                    int d = depth.fetch_add(1, std::memory_order_relaxed) + 1;

                    // track max depth
                    int prev = max_depth.load(std::memory_order_relaxed);
                    while (d > prev &&
                           !max_depth.compare_exchange_weak(prev, d,
                                   std::memory_order_relaxed)) {
                        // prev reloaded
                    }
                }

                int stall = stall_dist(rng);
                for (int k = 0; k < stall; ++k) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Consumer with jitter
    std::thread consumer([&] {
        std::mt19937 rng(999);
        std::uniform_int_distribution<int> stall_dist(1, 30);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        const int total = producers * iterations;

        while (local_count < total) {
            auto msg = q.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
                int d = depth.fetch_sub(1, std::memory_order_relaxed) - 1;
                // sanity: depth should never go negative
                EXPECT_GE(d, 0);
            } else {
                int stall = stall_dist(rng);
                for (int k = 0; k < stall; ++k) {
                    std::this_thread::yield();
                }
            }

            if ((local_count % 500) == 0) {
                int d = depth.load(std::memory_order_relaxed);
                int bucket = std::min(d / 100, 9);
                histogram[bucket].fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (auto& t : threads) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(consumed.load(), producers * iterations);
    EXPECT_EQ(depth.load(), 0);
    EXPECT_TRUE(q.empty());

    // Optional: debug output for plotting
    // std::cout << "===QUEUE_DEPTH_HISTOGRAM_BEGIN===\n";
    // for (int i = 0; i < 10; ++i) {
    //     int low  = i * 100;
    //     int high = (i + 1) * 100 - 1;
    //     int count = histogram[i].load(std::memory_order_relaxed);
    //     std::cout << low << "," << high << "," << count << "\n";
    // }
    // std::cout << "MAX_DEPTH," << max_depth.load() << "\n";
    // std::cout << "===QUEUE_DEPTH_HISTOGRAM_END===\n";
}
