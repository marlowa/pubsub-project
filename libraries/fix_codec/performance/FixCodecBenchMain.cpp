// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Micro-benchmark for the FIX codec hot path: how fast a NewOrderSingle is framed,
// read, and fully validated. NewOrderSingle is the message the gateway receives on
// every inbound order, so its parse-plus-validate cost is the figure that matters.
//
// Two measurements are reported:
//   parse           -- frame the message and read its key fields (no validation)
//   parse+validate  -- the above plus full dictionary validation (required tags,
//                      duplicates, data formats, enum membership)
//
// The executable installs to bin and is deliberately a tight, long-running loop so
// it can be profiled directly under perf, e.g.:
//   perf record -g --call-graph dwarf -- ./fix_codec_bench
//   perf report --stdio

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

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
    std::printf("%-16s avg %5lld ns   min %5lld ns   max %6lld ns   %10.0f msg/s\n", name, avg_ns, min_ns, max_ns, per_second);
}

} // namespaces

int main() {
    char buffer[256];
    const std::string_view wire = build_new_order_single(buffer, sizeof(buffer));
    if (wire.empty()) {
        std::printf("failed to build NewOrderSingle\n");
        return 1;
    }

    const int iterations = 2000000;
    uint64_t sink = 0;

    long long min_ns = 0;
    long long max_ns = 0;

    const long long parse_avg = measure_avg_ns(
        [&] {
            fix_codec::FixMessageReader reader(wire);
            sink += static_cast<uint64_t>(reader.status());
            sink += reader.find(tag::ClOrdID).as_string_view().size();
            sink += static_cast<uint64_t>(reader.find(tag::Side).as_char());
        },
        iterations, min_ns, max_ns);
    report("parse", parse_avg, min_ns, max_ns);

    const long long validate_avg = measure_avg_ns(
        [&] {
            fix_codec::FixMessageReader reader(wire);
            const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
            sink += static_cast<uint64_t>(reject.reason);
            sink += static_cast<uint64_t>(reject.ref_tag);
        },
        iterations, min_ns, max_ns);
    report("parse+validate", validate_avg, min_ns, max_ns);

    // Consume sink so the work above cannot be optimised away.
    if (sink == 0xFFFFFFFFFFFFFFFFULL) {
        std::printf("unreachable %llu\n", static_cast<unsigned long long>(sink));
    }
    return 0;
}
