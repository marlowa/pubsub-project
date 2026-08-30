// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OrderBookBench.cpp
 * @brief What the open-order region costs the matching engine's thread.
 *
 * The book used to be a map holding the orders themselves. It is now a map holding record
 * indices into a memory-mapped region, so every accept writes a record and publishes a
 * position, and every cancel releases one. That is work the engine did not do before, and the
 * design says it must be constant and small. This measures it.
 *
 * Three things are reported, per operation, as percentiles:
 *
 *   - the book as it was, a map holding the orders: the baseline
 *   - the book as it is, a map of indices over the region
 *   - the same again on a region whose pages have been touched first, which is what warm()
 *     does before the engine reports itself ready
 *
 * The difference between the second and the third is the cost of a cold page, which is the
 * one the engine must not pay on an order.
 *
 * Run it on a quiet machine, and against a path on the filesystem the venue is deployed to.
 * The absolute figures move with the machine; the difference between the runs is what the
 * design is asking about.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/IncrementalRehashMap.hpp>

#include "OrderBook.hpp"
#include "OrderEntry.hpp"
#include "OrderKey.hpp"

namespace {

using matching_engine::OrderBook;
using matching_engine::OrderEntry;
using matching_engine::OrderKey;
using matching_engine::OrderKeyHash;
using Clock = std::chrono::steady_clock;

constexpr size_t order_count = 1000000;

struct Percentiles {
    double mean_ns{};
    int64_t p50{};
    int64_t p90{};
    int64_t p99{};
    int64_t p999{};
    int64_t max{};
};

Percentiles summarise(std::vector<int64_t>& samples) {
    Percentiles out;
    if (samples.empty()) {
        return out;
    }
    long double total = 0;
    for (const int64_t sample : samples) {
        total += static_cast<long double>(sample);
    }
    out.mean_ns = static_cast<double>(total / static_cast<long double>(samples.size()));
    std::sort(samples.begin(), samples.end());
    const auto at = [&samples](double fraction) {
        size_t index = static_cast<size_t>(fraction * static_cast<double>(samples.size()));
        if (index >= samples.size()) {
            index = samples.size() - 1;
        }
        return samples[index];
    };
    out.p50 = at(0.50);
    out.p90 = at(0.90);
    out.p99 = at(0.99);
    out.p999 = at(0.999);
    out.max = samples.back();
    return out;
}

void report(const char* what, Percentiles p) {
    fmt::print("  {:<34}  mean {:7.1f}   p50 {:5}   p90 {:5}   p99 {:6}   p99.9 {:7}   max {:9}\n", what, p.mean_ns, p.p50, p.p90, p.p99, p.p999, p.max);
}

// The cost of asking the clock twice, which every sample below carries. Reported so that the
// per-operation figures can be read for what they are.
int64_t measure_clock_overhead() {
    std::vector<int64_t> samples;
    samples.reserve(100000);
    for (size_t i = 0; i < 100000; ++i) {
        const auto start = Clock::now();
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

std::vector<OrderKey> build_keys() {
    std::vector<OrderKey> keys;
    keys.reserve(order_count);
    const fix_common::SessionIdentity session = fix_common::SessionIdentity::make("BENCH-MEMBER", 1);
    for (size_t i = 0; i < order_count; ++i) {
        keys.push_back(OrderKey::make(session, "ORD-" + std::to_string(i)));
    }
    return keys;
}

OrderEntry sample_entry(int64_t order_id_num) {
    OrderEntry entry{};
    entry.order_id_num = order_id_num;
    entry.session = fix_common::SessionIdentity::make("BENCH-MEMBER", 1);
    entry.side = pubsub_itc_fw_app::Side::Buy;
    entry.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    entry.has_price = true;
    entry.set_symbol("BHP");
    entry.set_order_qty("100");
    entry.set_price("42.50");
    return entry;
}

// The book as it was: a map holding the orders themselves, and nothing written anywhere that
// outlives the process.
void bench_map_only(const std::vector<OrderKey>& keys, const OrderEntry& entry) {
    pubsub_itc_fw::IncrementalRehashMap<OrderKey, OrderEntry, OrderKeyHash> book(0, OrderKeyHash{}, std::equal_to<OrderKey>{}, nullptr);
    book.reserve(order_count);

    std::vector<int64_t> accepts;
    accepts.reserve(order_count);
    for (size_t i = 0; i < order_count; ++i) {
        const auto start = Clock::now();
        book.emplace(keys[i], entry);
        const auto end = Clock::now();
        accepts.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    std::vector<int64_t> cancels;
    cancels.reserve(order_count);
    for (size_t i = 0; i < order_count; ++i) {
        const auto start = Clock::now();
        const auto it = book.find(keys[i]);
        const OrderEntry found = it->second;
        book.erase(keys[i]);
        const auto end = Clock::now();
        cancels.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        if (found.order_id_num == -1) {
            std::abort(); // never; keeps the read from being optimised away
        }
    }

    fmt::print("the book as it was -- a map holding the orders\n");
    report("accept: emplace", summarise(accepts));
    report("cancel: find, copy, erase", summarise(cancels));
}

// The book as it is: a map of record indices, with the order itself in the region.
void bench_with_region(const std::string& path, const std::vector<OrderKey>& keys, const OrderEntry& entry, bool warm_first, const char* title) {
    ::unlink(path.c_str());
    OrderBook book;
    static_cast<void>(book.open(path, static_cast<OrderBook::SlotIndex>(order_count), order_count));

    int64_t warm_ns = 0;
    if (warm_first) {
        const auto start = Clock::now();
        book.warm();
        const auto end = Clock::now();
        warm_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }

    std::vector<int64_t> accepts;
    accepts.reserve(order_count);
    for (size_t i = 0; i < order_count; ++i) {
        const auto start = Clock::now();
        const bool added = book.add(keys[i], entry, static_cast<int64_t>(i) + 1);
        book.publish(static_cast<int64_t>(i) + 1);
        const auto end = Clock::now();
        accepts.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        if (!added) {
            std::abort(); // the region was sized to hold them all
        }
    }

    std::vector<int64_t> cancels;
    cancels.reserve(order_count);
    for (size_t i = 0; i < order_count; ++i) {
        const auto start = Clock::now();
        const OrderEntry* found = book.find(keys[i]);
        const OrderEntry copied = *found;
        book.remove(keys[i]);
        book.publish(static_cast<int64_t>(order_count + i) + 1);
        const auto end = Clock::now();
        cancels.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        if (copied.order_id_num == -1) {
            std::abort(); // never; keeps the read from being optimised away
        }
    }

    fmt::print("{}\n", title);
    if (warm_first) {
        fmt::print("  warm() over the whole region: {} ns\n", warm_ns);
    }
    report("accept: add, publish", summarise(accepts));
    report("cancel: find, copy, remove, publish", summarise(cancels));
    ::unlink(path.c_str());
}

// What a successor process pays for a region a previous one left behind.
//
// The runs above create the file, so every page of it is a hole: touching one costs a minor
// fault and reads nothing, because there is nothing on the disk to read. That is not the case
// a restart faces. A region written by a process that then died holds real pages, and once the
// page cache has given them up -- which a busy machine will do -- reading one is a disk read.
// This writes the region, pushes it to the disk, tells the kernel to forget it, and then
// measures what touching it costs.
void bench_after_eviction(const std::string& path, const std::vector<OrderKey>& keys, const OrderEntry& entry) {
    ::unlink(path.c_str());
    {
        OrderBook book;
        static_cast<void>(book.open(path, static_cast<OrderBook::SlotIndex>(order_count), order_count));
        for (size_t i = 0; i < order_count; ++i) {
            static_cast<void>(book.add(keys[i], entry, static_cast<int64_t>(i) + 1));
        }
        book.publish(static_cast<int64_t>(order_count));
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fmt::print("a region left behind: cannot reopen {}\n", path);
        return;
    }
    ::fsync(fd);
    const int forgotten = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);

    OrderBook book;
    static_cast<void>(book.open(path, static_cast<OrderBook::SlotIndex>(order_count), order_count));
    const auto start = Clock::now();
    book.warm();
    const auto end = Clock::now();
    const int64_t warm_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    fmt::print("a region left behind by a previous process, dropped from the page cache\n");
    if (forgotten != 0) {
        fmt::print("  the kernel would not drop it, so this is a warm cache and says nothing\n");
    }
    fmt::print("  warm() over the whole region: {} ns ({:.1f} ms)\n", warm_ns, static_cast<double>(warm_ns) / 1e6);
    fmt::print("  which is {:.0f} ns a page over {} pages\n", static_cast<double>(warm_ns) / static_cast<double>((order_count * sizeof(OrderEntry)) / 4096),
               (order_count * sizeof(OrderEntry)) / 4096);
    ::unlink(path.c_str());
}

} // namespaces

int main(int argc, char** argv) {
    // Under the deployment's own directory, as every file this project writes is, and not a
    // machine-wide tmpfs. It matters more here than elsewhere: tmpfs pages never reach a disk,
    // so measuring the region there would report a cost the deployed venue does not pay.
    const std::string path = argc > 1 ? argv[1] : "var/order_book_bench.region";

    fmt::print("order_book_bench: {} orders, one record of {} bytes each, region {:.0f} MB\n", order_count, sizeof(OrderEntry),
               static_cast<double>(order_count * sizeof(OrderEntry)) / (1024.0 * 1024.0));
    fmt::print("clock overhead carried by every sample below: {} ns\n\n", measure_clock_overhead());

    const std::vector<OrderKey> keys = build_keys();
    const OrderEntry entry = sample_entry(1);

    bench_map_only(keys, entry);
    fmt::print("\n");
    bench_with_region(path, keys, entry, false, "the book as it is -- a map of record indices, region untouched");
    fmt::print("\n");
    bench_with_region(path, keys, entry, true, "the same, with the region touched first (what warm() does)");
    fmt::print("\n");
    bench_after_eviction(path, keys, entry);
    return 0;
}
