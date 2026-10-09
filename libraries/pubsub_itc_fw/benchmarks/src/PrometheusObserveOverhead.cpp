// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * What it costs to record one value in a histogram on a hot path, with and without a virtual
 * function call.
 *
 * Two ways of recording are compared. Both record into the same kind of histogram,
 * pubsub_itc_fw::SingleWriterHistogram, with the same eighteen bucket bounds, so the work of
 * recording is identical and only the way it is reached differs.
 *
 *   Through a virtual call. InterfaceHistogramHandle holds a pointer to an abstract
 *   HistogramInterface; observe() tests the pointer for null and makes a virtual call to the
 *   implementation, which records the value.
 *
 *   Through the concrete handle. pubsub_itc_fw::HistogramHandle, obtained from a
 *   PrometheusEndpoint with metrics enabled, holds a pointer to the SingleWriterHistogram itself;
 *   observe() tests the pointer for null and records the value, and the compiler builds all of
 *   that into the caller, so no call is made at all.
 *
 * A third benchmark runs the same loop recording nothing, to show what the loop itself costs.
 *
 * The histograms are created in a different source file (HistogramsUnderTest.cpp) and reached
 * only through functions declared in its header. That keeps from this file the knowledge of which
 * class implements HistogramInterface; with it, the compiler could replace the virtual call with a
 * direct one, and the first benchmark would not measure a virtual call. Whether each loop does
 * what it says can be checked in the machine code, for example with
 *   objdump -d --no-show-raw-insn -C prometheus_observe_overhead
 * where the loop of the first benchmark holds an indirect call ("call *...") and the loop of the
 * second holds none.
 *
 * Each benchmark records a different value each time, taken in turn from 4096 values spread across
 * the buckets, so that the search for the bucket does not always end in the same place. After each
 * value, benchmark::ClobberMemory() tells the compiler that memory may have changed, so it reads the
 * handle afresh every time, as code that records one value among other work does, rather than
 * keeping it in a register for the whole loop.
 *
 * Run it pinned to one core, on a machine set up for measuring, from the installation directory.
 * Like every program the venue installs, it finds the framework's shared library through
 * LD_LIBRARY_PATH, which the launch scripts set for the venue's own programs:
 *   python3 scripts/measurement_machine.py --on      (as root)
 *   LD_LIBRARY_PATH=lib taskset -c 6 bin_benchmarks/prometheus_observe_overhead
 * Each benchmark is repeated ten times, and the median, the mean, the standard deviation and the
 * coefficient of variation across the ten are reported.
 *
 * A caution about reading the results. A difference of a nanosecond or less between two ways of
 * recording may not be the way of recording at all. Where the compiler places a loop in memory
 * can change its time per pass by about a nanosecond, and which of two loops comes out faster has
 * been seen to depend on that placement alone. Repetitions within one run do not reveal this,
 * because every repetition uses the same placement; building again after an unrelated change can.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include <benchmark/benchmark.h>

#include <HistogramsUnderTest.hpp>
#include <InterfaceHistogramHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>

namespace {

constexpr size_t value_count = 4096;
constexpr size_t value_index_mask = value_count - 1;

// Values in nanoseconds from 50 to about 200,000, so that most of the eighteen buckets are used.
// A fixed sequence, the same on every run, from a simple linear congruential generator.
std::array<double, value_count> make_values() {
    std::array<double, value_count> values{};
    uint32_t state = 12345;
    for (double& value : values) {
        state = state * 1103515245U + 12345U;
        value = 50.0 + static_cast<double>((state >> 8) % 200000U);
    }
    return values;
}

const std::array<double, value_count> values = make_values();

void empty_loop(benchmark::State& state) {
    size_t index = 0;
    for (auto _ : state) {
        double value = values[index & value_index_mask];
        benchmark::DoNotOptimize(value);
        benchmark::ClobberMemory();
        ++index;
    }
}

void observe_through_virtual_call(benchmark::State& state) {
    pubsub_itc_fw_benchmarks::InterfaceHistogramHandle handle = pubsub_itc_fw_benchmarks::interface_histogram_handle();
    size_t index = 0;
    for (auto _ : state) {
        handle.observe(values[index & value_index_mask]);
        benchmark::ClobberMemory();
        ++index;
    }
}

void observe_through_concrete_handle(benchmark::State& state) {
    pubsub_itc_fw::HistogramHandle handle = pubsub_itc_fw_benchmarks::concrete_histogram_handle();
    size_t index = 0;
    for (auto _ : state) {
        handle.observe(values[index & value_index_mask]);
        benchmark::ClobberMemory();
        ++index;
    }
}

constexpr int repetitions = 10;

} // un-named namespace

BENCHMARK(empty_loop)->Repetitions(repetitions)->DisplayAggregatesOnly(true);
BENCHMARK(observe_through_virtual_call)->Repetitions(repetitions)->DisplayAggregatesOnly(true);
BENCHMARK(observe_through_concrete_handle)->Repetitions(repetitions)->DisplayAggregatesOnly(true);

BENCHMARK_MAIN();
