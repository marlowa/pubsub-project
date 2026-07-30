// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Micro-benchmark for the FIX codec hot path: how fast a NewOrderSingle is framed,
// read, and fully validated. NewOrderSingle is the message the gateway receives on
// every inbound order, so its parse-plus-validate cost is the figure that matters.
//
// Three measurements are reported:
//   frame only      -- frame the message and verify its checksum, reading no fields
//   parse           -- frame the message and read its key fields (no validation)
//   parse+validate  -- the above plus full dictionary validation (required tags,
//                      duplicates, data formats, enum membership)
//
// Note when comparing "frame only" against "parse": the parse figure reads its two
// fields with unhinted find() calls, each of which tokenises again from the start of
// the message. The gateway makes a single pass instead, so the difference between the
// two lines overstates what it pays to read fields.
//
// Timing is batched by default -- one clock reading per batch of iterations, fastest
// batch reported -- because a clock_gettime pair costs a large fraction of the couple
// of hundred nanoseconds being measured. Pass --per-iteration for the original
// per-iteration timing, whose figures carry that overhead and a much wider spread.
// Pin the process for a stable comparison: taskset -c 2 ./fix_codec_bench
//
// The executable installs to bin and is deliberately a tight, long-running loop so
// it can be profiled directly under perf, e.g.:
//   perf record -g --call-graph dwarf -- ./fix_codec_bench
//   perf report --stdio

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixMessageValidator.hpp>
#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/FixReject.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace tag = fix_codec::tag;

namespace {

template <typename Function> long long measure_avg_ns(Function&& function, int iterations, long long& min_ns, long long& max_ns) {
    for (int warmup = 0; warmup < 1000; ++warmup) {
        function();
    }
    min_ns = std::numeric_limits<long long>::max();
    max_ns = 0;
    long long total = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto start = std::chrono::high_resolution_clock::now();
        function();
        const auto end = std::chrono::high_resolution_clock::now();
        const long long elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        total += elapsed;
        min_ns = std::min(min_ns, elapsed);
        max_ns = std::max(max_ns, elapsed);
    }
    return total / iterations;
}

/**
 * @brief Times a batch of iterations per clock reading and returns the fastest batch.
 *
 * @param[in]  function The work to measure, called batch_size times per reading.
 * @param[in]  batch_size    Iterations between the two clock readings.
 * @param[in]  batch_count   Number of batches; the fastest is reported.
 * @return The mean nanoseconds per iteration of the fastest batch.
 *
 * The parse path costs a couple of hundred nanoseconds, and a clock_gettime pair
 * costs a sizeable fraction of that. Reading the clock once per iteration therefore
 * charges every measurement for the instrument and leaves a noise floor wider than
 * the differences worth chasing. Amortising one reading over a batch removes both.
 * The fastest batch is reported rather than the mean because it is the sample least
 * disturbed by scheduling and frequency scaling.
 */
template <typename Function> double measure_batched_ns(Function&& function, int batch_size, int batch_count) {
    for (int warmup = 0; warmup < batch_size; ++warmup) {
        function();
    }
    double best = std::numeric_limits<double>::max();
    for (int batch = 0; batch < batch_count; ++batch) {
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < batch_size; ++iteration) {
            function();
        }
        const auto end = std::chrono::steady_clock::now();
        const double elapsed = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        best = std::min(best, elapsed / batch_size);
    }
    return best;
}

std::string_view build_new_order_single(char* buffer, size_t capacity) {
    fix_codec::FixMessageWriter writer(buffer, capacity);
    writer.push_back_field(tag::MsgType, fix_codec::msg_type::NewOrderSingle);
    writer.push_back_field(tag::SenderCompID, std::string_view("GATEWAY"));
    writer.push_back_field(tag::TargetCompID, std::string_view("CLIENT"));
    writer.push_back_field(tag::MsgSeqNum, 1);
    writer.push_back_field(tag::SendingTime, std::string_view("20260721-10:00:00"));
    writer.push_back_field(tag::ClOrdID, std::string_view("ORDER-1"));
    writer.push_back_field(tag::Side, '1');
    writer.push_back_field(tag::OrdType, '2');
    writer.push_back_field(tag::TransactTime, std::string_view("20260721-10:00:00"));
    writer.push_back_field(tag::OrderQty, std::string_view("100"));
    writer.push_back_field(tag::Price, std::string_view("42.50"));
    return writer.finish();
}

void report(const char* name, long long avg_ns, long long min_ns, long long max_ns) {
    const double per_second = avg_ns > 0 ? 1e9 / static_cast<double>(avg_ns) : 0.0;
    fmt::print("{:<16} avg {:5} ns   min {:5} ns   max {:6} ns   {:10.0f} msg/s\n", name, avg_ns, min_ns, max_ns, per_second);
}

void report_batched(const char* name, double nanoseconds) {
    const double per_second = nanoseconds > 0.0 ? 1e9 / nanoseconds : 0.0;
    fmt::print("{:<16} {:7.1f} ns   {:10.0f} msg/s\n", name, nanoseconds, per_second);
}

} // namespaces

int main(int argc, char** argv) {
    bool per_iteration_mode = false;
    for (int argument = 1; argument < argc; ++argument) {
        const std::string_view option(argv[argument]);
        if (option == "--per-iteration") {
            per_iteration_mode = true;
        } else {
            fmt::print("usage: {} [--per-iteration]\n", argv[0]);
            fmt::print("  default        time a batch of iterations per clock reading (accurate)\n");
            fmt::print("  --per-iteration  read the clock around every iteration (legacy; the\n");
            fmt::print("                   reading itself costs a large fraction of the result)\n");
            return option == "--help" ? 0 : 2;
        }
    }

    char buffer[256];
    const std::string_view wire = build_new_order_single(buffer, sizeof(buffer));
    if (wire.empty()) {
        fmt::print("failed to build NewOrderSingle\n");
        return 1;
    }

    uint64_t sink = 0;

    const auto frame_only = [&] {
        fix_codec::FixMessageReader reader(wire);
        sink += static_cast<uint64_t>(reader.status());
    };
    const auto parse = [&] {
        fix_codec::FixMessageReader reader(wire);
        sink += static_cast<uint64_t>(reader.status());
        sink += reader.find(tag::ClOrdID).as_string_view().size();
        sink += static_cast<uint64_t>(reader.find(tag::Side).as_char());
    };
    const auto parse_and_validate = [&] {
        fix_codec::FixMessageReader reader(wire);
        const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
        sink += static_cast<uint64_t>(reject.reason);
        sink += static_cast<uint64_t>(reject.ref_tag);
    };

    if (per_iteration_mode) {
        const int iterations = 2000000;
        long long min_ns = 0;
        long long max_ns = 0;
        const long long parse_avg = measure_avg_ns(parse, iterations, min_ns, max_ns);
        report("parse", parse_avg, min_ns, max_ns);
        const long long validate_avg = measure_avg_ns(parse_and_validate, iterations, min_ns, max_ns);
        report("parse+validate", validate_avg, min_ns, max_ns);
    } else {
        const int batch_size = 20000;
        const int batch_count = 200;
        fmt::print("message {} bytes, fastest of {} batches of {}\n", wire.size(), batch_count, batch_size);
        report_batched("frame only", measure_batched_ns(frame_only, batch_size, batch_count));
        report_batched("parse", measure_batched_ns(parse, batch_size, batch_count));
        report_batched("parse+validate", measure_batched_ns(parse_and_validate, batch_size, batch_count));
    }

    // Consume sink so the work above cannot be optimised away.
    if (sink == 0xFFFFFFFFFFFFFFFFULL) {
        fmt::print("unreachable {}\n", sink);
    }
    return 0;
}
