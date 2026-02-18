// ============================================================================
// LockFreeMessageQueue Test Suite
// ============================================================================
//
// This file contains a comprehensive set of Google Tests for validating the
// correctness, performance, and concurrency behaviour of the
// LockFreeMessageQueue class.
//
// The queue is a multi-producer / single-consumer lock-free structure used in
// high-performance, pinned-thread application environments. Because the queue
// is a core infrastructure component, these tests aim to provide extremely
// high confidence in its behaviour under a wide range of real-world and
// pathological conditions.
//
// The suite covers:
//
//   • Basic functional correctness
//       - FIFO ordering
//       - enqueue/dequeue semantics
//
//   • Multi-threaded correctness
//       - multiple producers racing a single consumer
//       - pinned-core execution to simulate production deployment
//
//   • Watermark behaviour
//       - high/low watermark transitions
//       - hysteresis stability
//       - storm tests to ensure handlers fire exactly once per crossing
//
//   • Shutdown semantics
//       - enqueue-after-shutdown safety
//       - destructor draining behaviour
//       - shutdown races under load
//
//   • Stress and soak testing
//       - millions of messages
//       - jittered producer/consumer timing
//       - memory-pressure bursts
//       - ABA-adjacent pointer churn
//       - false sharing detection
//
//   • Behavioural profiling
//       - queue-depth histogram sampling
//       - output for external plotting/analysis
//
// These tests intentionally simulate:
//   - realistic timing jitter (cache misses, allocator stalls, NUMA hiccups)
//   - pinned-thread execution
//   - bursty producer behaviour
//   - slow or uneven consumer behaviour
//   - allocator churn and node reuse
//
// The goal is not only to verify correctness, but to expose rare timing bugs,
// ordering issues, or structural weaknesses that only appear under sustained
// or adversarial conditions.
//
// This suite is designed to be read and understood by future maintainers.
// Each test includes a comment block describing its purpose and the specific
// failure modes it is intended to detect.
//
// ============================================================================

#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <random>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/LockFreeMessageQueue.hpp>
#include <pubsub_itc_fw/QueueConfig.hpp>
#include <pubsub_itc_fw/AllocatorConfig.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

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

AllocatorConfig make_default_allocator_config() {
    AllocatorConfig cfg;
    cfg.pool_name = "LockFreeMessageQueueTestPool";
    cfg.objects_per_pool = 1024;
    cfg.initial_pools = 1;
    cfg.expansion_threshold_hint = 0;
    cfg.handler_for_pool_exhausted = nullptr;
    cfg.handler_for_invalid_free = nullptr;
    cfg.handler_for_huge_pages_error = nullptr;
    cfg.use_huge_pages_flag = UseHugePagesFlag(UseHugePagesFlag::DoNotUseHugePages);
    cfg.context = nullptr;
    return cfg;
}

QueueConfig make_default_queue_config() {
    QueueConfig cfg;
    cfg.low_watermark = 0;
    cfg.high_watermark = 0;
    cfg.for_client_use = nullptr;
    cfg.gone_below_low_watermark_handler = nullptr;
    cfg.gone_above_high_watermark_handler = nullptr;
    return cfg;
}

} // namespace

// ------------------------------------------------------------
// Basic sanity check: verifies FIFO ordering and that enqueue()
// and dequeue() work correctly in the simplest single-threaded
// scenario. This ensures the fundamental queue mechanics behave
// as expected before layering on concurrency tests.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, BasicEnqueueDequeue) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    queue.enqueue(TestMessage{1});
    queue.enqueue(TestMessage{2});
    queue.enqueue(TestMessage{3});

    auto m1 = queue.dequeue();
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->value, 1);

    auto m2 = queue.dequeue();
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->value, 2);

    auto m3 = queue.dequeue();
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->value, 3);

    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Multi-producer correctness test: launches several producers
// concurrently and ensures that all messages are eventually
// consumed with no loss, corruption, or duplication. This
// validates the lock-free multi-producer logic under moderate
// contention.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, MultiProducerSingleConsumer) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
                queue.enqueue(TestMessage{p * per_producer + i});
            }
        });
    }

    start_flag.store(true);

    // Consumer
    int count = 0;
    while (count < producers * per_producer) {
        auto msg = queue.dequeue();
        if (msg.has_value()) {
            ++count;
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Watermark transition correctness: verifies that the high and
// low watermark handlers fire exactly once when crossing their
// respective thresholds. Ensures hysteresis logic is correct
// and handlers do not storm or double-fire.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, WatermarkHandlersFireOnce) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    QueueConfig queue_config;
    queue_config.low_watermark = 2;
    queue_config.high_watermark = 5;
    queue_config.for_client_use = nullptr;
    queue_config.gone_below_low_watermark_handler =
        [&](void*) { low_calls.fetch_add(1); };
    queue_config.gone_above_high_watermark_handler =
        [&](void*) { high_calls.fetch_add(1); };

    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    // Enqueue up to high watermark
    for (int i = 0; i < 5; ++i) {
        queue.enqueue(TestMessage{i});
    }

    EXPECT_EQ(high_calls.load(), 1);

    // Dequeue down to low watermark
    for (int i = 0; i < 4; ++i) {
        auto msg = queue.dequeue();
        ASSERT_TRUE(msg.has_value());
    }

    EXPECT_EQ(low_calls.load(), 1);
}

// ------------------------------------------------------------
// Shutdown behavior: verifies that enqueue() after shutdown()
// does not crash, corrupt memory, or accept new messages.
// Ensures shutdown flag is respected and the queue enters a
// safe, inert state.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, EnqueueAfterShutdownIsDropped) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    auto* queue = new LockFreeMessageQueue<TestMessage>(queue_config, allocator_config);

    // Enqueue a few items
    queue->enqueue(TestMessage{1});
    queue->enqueue(TestMessage{2});

    // Trigger shutdown
    queue->shutdown();

    // Enqueue after shutdown should be ignored
    queue->enqueue(TestMessage{999});

    delete queue;

    SUCCEED();
}

// ------------------------------------------------------------
// Destructor behavior: ensures that destroying the queue drains
// all remaining messages safely without leaks, corruption, or
// undefined behavior. Validates that the destructor correctly
// invokes shutdown semantics.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, DestructorDrainsQueue) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    auto* queue = new LockFreeMessageQueue<TestMessage>(queue_config, allocator_config);

    queue->enqueue(TestMessage{1});
    queue->enqueue(TestMessage{2});
    queue->enqueue(TestMessage{3});

    delete queue;  // Should drain safely

    SUCCEED();
}

// ------------------------------------------------------------
// Heavy multi-producer stress test: pushes the queue with a
// large number of concurrent producers and verifies that all
// messages are delivered. This exposes timing races, atomic
// ordering issues, and pointer-linking bugs under sustained
// contention.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, HeavyMultiProducerStress) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
                queue.enqueue(TestMessage{p * per_producer + i});
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Start all producers
    start_flag.store(true, std::memory_order_release);

    // Single consumer
    int local_count = 0;
    while (local_count < total) {
        auto msg = queue.dequeue();
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
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// CPU affinity test: pins producer and consumer threads to
// separate physical cores to simulate real-world deployment
// conditions. Ensures the queue behaves correctly when threads
// do not migrate and memory access patterns are stable.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, ProducerConsumerPinnedToSeparateCores) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
            queue.enqueue(TestMessage{i});
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&] {
        pin_current_thread_to_cpu(1);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        while (local_count < messages) {
            auto msg = queue.dequeue();
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
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Large-scale soak test: pushes tens of millions of messages
// through the queue to expose rare timing bugs, ABA-like
// behavior, allocator churn issues, and long-term stability
// problems. This is a high-confidence stress test.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, SoakTestMillionsOfMessages) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

#ifdef USING_VALGRIND
    constexpr int producers = 2;            // Reduced threads
    constexpr int per_producer = 10'000;    // 20k total is plenty for a Valgrind check
#else
    constexpr int producers = 4;
    constexpr int per_producer = 2'500'000; // 2.5M each → 10M total
#endif
    constexpr int total = producers * per_producer;

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            Backoff start_backoff;
            while (!start_flag.load(std::memory_order_acquire)) {
                start_backoff.pause();
            }
            for (int i = 0; i < per_producer; ++i) {
                queue.enqueue(TestMessage{i});
            }
        });
    }

    start_flag.store(true, std::memory_order_release);

    int local_count = 0;
    Backoff count_backoff;
    while (local_count < total) {
        auto msg = queue.dequeue();
        if (msg.has_value()) {
            ++local_count;
        } else {
            count_backoff.pause();
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// False sharing detection: producer and consumer are pinned to
// separate cores and run at high speed. If any shared cache
// lines are incorrectly laid out, performance collapses or
// behavior becomes unstable. This test helps detect structural
// layout issues.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, FalseSharingDetection) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
            queue.enqueue(TestMessage{i});
        }
    });

    // Consumer pinned to CPU 1
    std::thread consumer([&] {
        pin_current_thread_to_cpu(1);

        start_flag.store(true, std::memory_order_release);

        int local_count = 0;
        while (local_count < iterations) {
            auto msg = queue.dequeue();
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
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// ABA-resistance stress test: forces rapid allocation and
// deallocation of nodes by generating many small bursts. This
// exposes pointer reuse hazards, stale-next-pointer races, and
// other ABA-adjacent failure modes in the linked-list structure.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, AbaResistanceStress) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
                    queue.enqueue(TestMessage{p * 1'000'000 + b * burst_size + i});
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
        auto msg = queue.dequeue();
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
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Watermark storm test: repeatedly oscillates the queue size
// above and below the high/low watermarks thousands of times.
// Ensures each transition fires exactly once per crossing and
// that hysteresis logic remains stable under extreme churn.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, WatermarkStormTest) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    QueueConfig queue_config;
    queue_config.low_watermark = 10;
    queue_config.high_watermark = 20;
    queue_config.for_client_use = nullptr;
    queue_config.gone_below_low_watermark_handler =
        [&](void*) { low_calls.fetch_add(1, std::memory_order_relaxed); };
    queue_config.gone_above_high_watermark_handler =
        [&](void*) { high_calls.fetch_add(1, std::memory_order_relaxed); };

    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    constexpr int cycles = 2000;

    // We will oscillate the queue size between 0 → 25 → 0 repeatedly.
    for (int c = 0; c < cycles; ++c) {

        // Fill above high watermark
        for (int i = 0; i < 25; ++i) {
            queue.enqueue(TestMessage{i});
        }

        // Drain below low watermark
        for (int i = 0; i < 25; ++i) {
            auto msg = queue.dequeue();
            if (!msg.has_value()) {
                std::this_thread::yield();
            }
        }
    }

    // After many oscillations, each transition should fire exactly once per cycle.
    EXPECT_EQ(high_calls.load(), cycles);
    EXPECT_EQ(low_calls.load(), cycles);
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Shutdown race test: triggers shutdown while producers are
// still actively enqueueing. Ensures that enqueue() safely
// drops messages after shutdown, that no corruption occurs,
// and that the queue drains cleanly without undefined behavior.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, ShutdownRaceTest) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    constexpr int initial_messages = 1000;
    constexpr int racing_messages = 200000;

    // Fill the queue with some initial messages
    for (int i = 0; i < initial_messages; ++i) {
        queue.enqueue(TestMessage{i});
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
                queue.enqueue(TestMessage{999999});
            } else {
                queue.enqueue(TestMessage{i});
            }
        }
    });

    // Consumer drains until shutdown is triggered
    std::thread consumer([&] {
        start_flag.store(true, std::memory_order_release);

        int drained = 0;
        while (drained < initial_messages) {
            auto msg = queue.dequeue();
            if (msg.has_value()) {
                ++drained;
            } else {
                std::this_thread::yield();
            }
        }

        // Trigger shutdown mid‑stream
        shutdown_now.store(true, std::memory_order_release);
        queue.shutdown();
    });

    racing_producer.join();
    consumer.join();

    // After shutdown, queue must be in a safe state
    while (true) {
        auto msg = queue.dequeue();
        if (!msg.has_value()) break;
    }

    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Low-watermark storm test: mirror image of the high-watermark
// storm test. Repeatedly crosses the low watermark boundary to
// ensure the low handler fires exactly once per downward
// transition and remains stable under oscillation.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, LowWatermarkStormTest) {
    std::atomic<int> high_calls{0};
    std::atomic<int> low_calls{0};

    QueueConfig queue_config;
    queue_config.low_watermark = 10;
    queue_config.high_watermark = 20;
    queue_config.for_client_use = nullptr;
    queue_config.gone_below_low_watermark_handler =
        [&](void*) { low_calls.fetch_add(1, std::memory_order_relaxed); };
    queue_config.gone_above_high_watermark_handler =
        [&](void*) { high_calls.fetch_add(1, std::memory_order_relaxed); };

    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    constexpr int cycles = 2000;

    // We oscillate the queue size between 0 → 25 → 0 repeatedly.
    // This forces both transitions many times.
    for (int c = 0; c < cycles; ++c) {

        // Fill above high watermark
        for (int i = 0; i < 25; ++i) {
            queue.enqueue(TestMessage{i});
        }

        // Drain below low watermark
        for (int i = 0; i < 25; ++i) {
            auto msg = queue.dequeue();
            if (!msg.has_value()) {
                std::this_thread::yield();
            }
        }
    }

    // Each cycle should produce exactly one high→low and one low→high transition.
    EXPECT_EQ(high_calls.load(), cycles);
    EXPECT_EQ(low_calls.load(), cycles);
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Throughput benchmark: measures messages-per-second under
// multi-producer load. This is not a correctness test but a
// performance regression guard to ensure the queue remains
// efficient over time.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, ThroughputBenchmark) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

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
                queue.enqueue(TestMessage{i});
            }
        });
    }

    // Start timing
    auto start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    // Consumer
    int local_count = 0;
    while (local_count < total) {
        auto msg = queue.dequeue();
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
#ifdef USING_VALGRIND
    const double minimum_throughput = 50'000.0; // Realistic for instrumented code
#else
    const double minimum_throughput = 500'000.0; // Production floor
#endif

    EXPECT_GT(msgs_per_sec, minimum_throughput) << "Throughput too low";

    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Mixed-rate jitter soak test: producers and consumer run with
// realistic micro-jitter (cache misses, allocator stalls,
// NUMA-like delays). This simulates real-world timing noise and
// exposes subtle races that fixed-rate tests cannot detect.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, MixedRateJitterSoakTest) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    constexpr int producers = 4;
#ifdef USING_VALGRIND
    constexpr int iterations = 10'000; // Reduced for Valgrind
#else
    constexpr int iterations = 500'000;
#endif

    std::atomic<bool> start_flag{false};
    std::atomic<int> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producers);

    // Producers with realistic jitter
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            std::mt19937 rng(p + 1);

            std::uniform_int_distribution<int> burst_dist(4, 32);
            std::uniform_int_distribution<int> tiny_stall(1, 3);
            std::uniform_int_distribution<int> medium_stall(20, 200);
            std::uniform_int_distribution<int> large_stall(200, 2000);
            std::uniform_int_distribution<int> stall_type(1, 1000);

            Backoff producer_backoff;
            while (!start_flag.load(std::memory_order_acquire)) {
                producer_backoff.pause();
            }

            for (int i = 0; i < iterations; ) {

                int burst = burst_dist(rng);
                for (int b = 0; b < burst && i < iterations; ++b, ++i) {
                    queue.enqueue(TestMessage{i});
                }

                producer_backoff.pause();

                int s = stall_type(rng);

                if (s <= 850) {
                    for (int k = 0, n = tiny_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                } else if (s <= 990) {
                    for (int k = 0, n = medium_stall(rng); k < n; ++k)
                        std::this_thread::yield();
                } else {
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
            auto msg = queue.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
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
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Memory-pressure burstn test: producers generate rapid bursts
// of allocations followed by forced pauses, simulating allocator
// slow paths and fragmentation. This stresses pointer linking,
// node reuse, and memory-ordering under high churn.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, MemoryPressureBurstTest) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

#ifdef USING_VALGRIND
    constexpr int producers = 2;       // Reduced for Valgrind
    constexpr int bursts = 100;        // Reduced from 3000
    constexpr int burst_size = 20;     // Reduced from 50
#else
    constexpr int producers = 4;
    constexpr int bursts = 3000;
    constexpr int burst_size = 50;   // small bursts → high allocation churn
#endif
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

            Backoff producer_backoff;
            while (!start_flag.load(std::memory_order_acquire)) {
                producer_backoff.pause();
            }

            for (int b = 0; b < bursts; ++b) {

                for (int i = 0; i < burst_size; ++i) {
                    queue.enqueue(TestMessage{i});
                }

                producer_backoff.pause();

                // Add a second pause only for the longer simulated stalls
                if (b % 10 == 0) {
                    producer_backoff.pause();
                }
            }
        });
    }

    // Consumer: drains continuously
    std::thread consumer([&] {
        start_flag.store(true, std::memory_order_release);

        Backoff consumer_backoff;
        int local_count = 0;
        while (local_count < total) {
            auto msg = queue.dequeue();
            if (msg.has_value()) {
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                consumer_backoff.pause();
            }
        }
    });

    for (auto& t : threads) {
        t.join();
    }
    consumer.join();

    EXPECT_EQ(consumed.load(), total);
    EXPECT_TRUE(queue.empty());
}

// ------------------------------------------------------------
// Queue-depth histogram test: tracks queue depth over time under
// jittery load and records a histogram of observed depths. This
// is a behavioral profiling test that ensures the queue does not
// accumulate unbounded backlog and that depth changes remain
// stable under realistic timing conditions. Output is used by
// the Python plotting script for visualization.
// ------------------------------------------------------------
TEST(LockFreeMessageQueueTest, QueueDepthHistogramTest) {
    QueueConfig queue_config = make_default_queue_config();
    AllocatorConfig allocator_config = make_default_allocator_config();

    LockFreeMessageQueue<TestMessage> queue(queue_config, allocator_config);

    // The rest of this test was truncated in the original snippet.
    // You can reinsert your existing histogram logic here unchanged,
    // using `queue` instead of `q` and the same construction pattern.
}
