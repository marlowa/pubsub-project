// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * What a spin loop costs, and what the PAUSE instruction in it is worth.
 *
 * A thread waiting for work can block, or it can spin. A thread that spins is usually told to
 * put a PAUSE instruction in the loop. This harness measures whether that advice holds on the
 * machine it is run on, because the answer depends on the processor generation and changed
 * sharply at one of them.
 *
 * Three things are measured.
 *
 *   How long one PAUSE takes. A loop with one is timed against the same loop without.
 *
 *   How long a spinning thread takes to notice that work has arrived. This is the only moment
 *   a waiting thread's speed matters, and a loop that spends most of its time inside a PAUSE
 *   checks less often, so it notices later.
 *
 *   How much work the other hardware thread of the same physical core gets done meanwhile.
 *   Two hardware threads share one core's execution resources, so a spinning thread competes
 *   with whatever is beside it. That is what PAUSE is meant to relieve.
 *
 * Run it pinned to two hardware threads of ONE physical core to measure the sharing, and to
 * two different cores as a control. Find the pairs with:
 *
 *     cat /sys/devices/system/cpu/cpu*\/topology/thread_siblings_list | sort -u
 *
 * Results are a property of the processor, not of this project, so they are not checked into
 * any expected-value file. Run it where the answer matters.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <immintrin.h>
#include <pthread.h>
#include <sched.h>

namespace {

std::atomic<bool> start_now{false};
std::atomic<bool> work_has_arrived{false};
std::atomic<int64_t> announced_at{0};
std::atomic<int64_t> noticed_at{0};
std::atomic<bool> stop_neighbour{false};
std::atomic<uint64_t> neighbour_rounds{0};

volatile uint64_t sink = 0;

int64_t now_ns() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/** @brief Times one iteration of a loop, with and without a PAUSE in it. */
double average_iteration_ns(int iterations, bool use_pause) {
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (use_pause) {
            _mm_pause();
        }
        sink = sink + 1;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}

/** @brief Spins until told work has arrived, then records when it noticed. */
void waiting_thread(int cpu, bool use_pause) {
    pin_to(cpu);
    while (!start_now.load(std::memory_order_acquire)) {}
    while (!work_has_arrived.load(std::memory_order_acquire)) {
        if (use_pause) {
            _mm_pause();
        }
    }
    noticed_at.store(now_ns(), std::memory_order_release);
}

/**
 * @brief Ordinary work on the other hardware thread, to see what the spinning costs it.
 *
 * It walks a block the size of a level-one data cache rather than working only in registers.
 * Register-only work is the case least likely to notice a spinning neighbour, so measuring
 * with it would flatter the spinner and say nothing about real code.
 */
void neighbouring_thread(int cpu) {
    pin_to(cpu);
    while (!start_now.load(std::memory_order_acquire)) {}
    std::vector<uint64_t> block(4096);
    for (size_t i = 0; i < block.size(); ++i) {
        block[i] = i * 2654435761ULL;
    }
    uint64_t value = 1;
    uint64_t rounds = 0;
    while (!stop_neighbour.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 1000; ++i) {
            const size_t index = value % block.size();
            value += block[index] + 1;
            block[index] = value;
        }
        ++rounds;
    }
    neighbour_rounds.store(rounds + (value & 1), std::memory_order_release);
}

double median_of(std::vector<double>& values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2];
}

/** @brief Measures noticing-latency and the neighbour's throughput for one loop style. */
void measure(int waiter_cpu, int neighbour_cpu, int rounds, bool use_pause) {
    std::vector<double> noticing;
    noticing.reserve(static_cast<size_t>(rounds));

    for (int round = 0; round < rounds; ++round) {
        work_has_arrived.store(false, std::memory_order_release);
        noticed_at.store(0, std::memory_order_release);
        start_now.store(false, std::memory_order_release);
        std::thread waiter(waiting_thread, waiter_cpu, use_pause);
        start_now.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        announced_at.store(now_ns(), std::memory_order_release);
        work_has_arrived.store(true, std::memory_order_release);
        waiter.join();
        noticing.push_back(static_cast<double>(noticed_at.load() - announced_at.load()));
    }

    work_has_arrived.store(false, std::memory_order_release);
    stop_neighbour.store(false, std::memory_order_release);
    neighbour_rounds.store(0, std::memory_order_release);
    start_now.store(false, std::memory_order_release);
    std::thread waiter(waiting_thread, waiter_cpu, use_pause);
    std::thread neighbour(neighbouring_thread, neighbour_cpu);
    start_now.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop_neighbour.store(true, std::memory_order_release);
    work_has_arrived.store(true, std::memory_order_release);
    neighbour.join();
    waiter.join();

    fmt::print("  {:<14} notices work after {:6.1f} ns    neighbour completed {:7} rounds\n", use_pause ? "with PAUSE" : "without PAUSE", median_of(noticing),
               neighbour_rounds.load());
}

} // namespaces

int main(int argc, char** argv) {
    const int waiter_cpu = argc > 1 ? atoi(argv[1]) : 2;
    const int neighbour_cpu = argc > 2 ? atoi(argv[2]) : 3;
    const int rounds = argc > 3 ? atoi(argv[3]) : 1500;

    fmt::print("Spin-wait harness: waiting thread on cpu{}, neighbour on cpu{}\n\n", waiter_cpu, neighbour_cpu);

    pin_to(waiter_cpu);
    average_iteration_ns(1000000, false); // warm up, and let the core reach full speed
    average_iteration_ns(1000000, true);
    const double without = average_iteration_ns(20000000, false);
    const double with = average_iteration_ns(20000000, true);

    fmt::print("What one PAUSE costs\n");
    fmt::print("  loop iteration without it : {:6.2f} ns\n", without);
    fmt::print("  loop iteration with it    : {:6.2f} ns\n", with);
    fmt::print("  so one PAUSE costs about  : {:6.2f} ns\n\n", with - without);

    // Off the waiting thread's processor before signalling anything. This thread pinned itself
    // above to time the PAUSE, and leaving it there would put the signaller and the waiter on
    // one processor, where they take turns instead of running together -- which measures the
    // scheduler rather than how quickly a spinning thread notices.
    const int signaller_cpu = (waiter_cpu == 0 || neighbour_cpu == 0) ? 1 : 0;
    pin_to(signaller_cpu);

    fmt::print("What that buys and what it costs (signalled from cpu{})\n", signaller_cpu);
    measure(waiter_cpu, neighbour_cpu, rounds, false);
    measure(waiter_cpu, neighbour_cpu, rounds, true);

    fmt::print("\nRun again with two processors on DIFFERENT physical cores as a control: where there\n");
    fmt::print("is no shared core there is no neighbour to relieve, and only the cost remains.\n");
    return 0;
}
