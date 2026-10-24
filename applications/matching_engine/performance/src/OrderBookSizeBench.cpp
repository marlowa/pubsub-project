// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OrderBookSizeBench.cpp
 * @brief What the order book costs in memory and in starting time, at the size a deployment configures.
 *
 * The book is two things: a memory-mapped region of fixed-size records, one per open order, and a map from
 * an order's identity to the number of its record. The region is sized by order_book_region_capacity and
 * the map is reserved for order_book_initial_capacity. This measures what each costs, so that the figures a
 * deployment is sized by are measured rather than worked out. Each measurement is one run of the program,
 * so that a recovery starts in a process that has never touched the region, as a restarted engine does.
 *
 *   order_book_size_bench map <orders>
 *       The resident memory the map takes when reserved for, and then filled with, this many orders.
 *
 *   order_book_size_bench create <region file> <records>
 *       How long creating a new region of this many records takes. The file must not exist.
 *
 *   order_book_size_bench fill <region file> <records> <orders>
 *       Adds this many open orders to an existing region and publishes them, so that a recovery finds them.
 *
 *   order_book_size_bench recover <region file> <records> <orders>
 *       How long opening an existing region and rebuilding the book from it takes, with the map reserved for
 *       this many orders. This is what an engine does when it restarts.
 *
 * Put the region file on the filesystem the venue is deployed to, never in /tmp, and delete it afterwards: a
 * region of 100 million records is 24.8 GB. A region larger than the memory the machine can spare is read
 * from and written to the disk while it is recovered, so its figures measure the disk; see
 * docs/bug_list.md, BUG-0028.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

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

// The process's resident memory in kilobytes, as the kernel reports it, or -1 if it cannot be read.
long resident_kilobytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::strtol(line.c_str() + 6, nullptr, 10);
        }
    }
    return -1;
}

// A distinct identity for the order numbered n, spread over a thousand members as a real day's orders are.
OrderKey key_for(long n) {
    const auto session = fix_common::SessionIdentity::make("MEMBER" + std::to_string(n % 1000), 1);
    return OrderKey::make(session, "CLORDID-" + std::to_string(n));
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int usage() {
    fmt::print(stderr, "usage: order_book_size_bench map <orders>\n"
                       "       order_book_size_bench create <region file> <records>\n"
                       "       order_book_size_bench fill <region file> <records> <orders>\n"
                       "       order_book_size_bench recover <region file> <records> <orders>\n");
    return 2;
}

int measure_map(long orders) {
    pubsub_itc_fw::IncrementalRehashMap<OrderKey, OrderBook::SlotIndex, OrderKeyHash> map;
    const long before = resident_kilobytes();
    map.reserve(static_cast<size_t>(orders));
    const long reserved = resident_kilobytes();
    for (long n = 0; n < orders; ++n) {
        map.emplace(key_for(n), static_cast<OrderBook::SlotIndex>(n));
    }
    const long filled = resident_kilobytes();
    fmt::print("map: {} orders in {} slots; resident memory {} kB before, {} kB once reserved, {} kB once filled; the map took {} kB\n", orders, map.capacity(),
               before, reserved, filled, filled - before);
    return 0;
}

int measure_create(const std::string& region, OrderBook::SlotIndex records) {
    OrderBook book;
    const auto start = Clock::now();
    if (book.open(region, records, 1)) {
        fmt::print(stderr, "create: {} already exists, so this would measure opening it, not creating it\n", region);
        return 1;
    }
    fmt::print("create: a region of {} records in {:.3f} s\n", records, seconds_since(start));
    return 0;
}

int fill(const std::string& region, OrderBook::SlotIndex records, long orders) {
    OrderBook book;
    book.open(region, records, static_cast<size_t>(orders));
    const auto start = Clock::now();
    OrderEntry entry{};
    for (long n = 0; n < orders; ++n) {
        entry.order_id_num = n;
        if (!book.add(key_for(n), entry, n + 1)) {
            fmt::print(stderr, "fill: the region was full after {} orders\n", n);
            return 1;
        }
    }
    book.publish(orders);
    fmt::print("fill: {} orders added in {:.3f} s\n", orders, seconds_since(start));
    return 0;
}

int measure_recover(const std::string& region, OrderBook::SlotIndex records, long orders) {
    OrderBook book;
    const auto start = Clock::now();
    book.open(region, records, static_cast<size_t>(orders));
    const double opened = seconds_since(start);
    const OrderBook::Recovery found = book.recover();
    fmt::print("recover: opened in {:.3f} s, and the book rebuilt in {:.3f} s in all, with {} open orders found and {} unfinished records discarded\n", opened,
               seconds_since(start), found.orders, found.discarded);
    return 0;
}

} // un-named namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const std::string mode = argv[1];
    if (mode == "map" && argc == 3) {
        return measure_map(std::atol(argv[2]));
    }
    if (argc < 4) {
        return usage();
    }
    const std::string region = argv[2];
    const auto records = static_cast<OrderBook::SlotIndex>(std::atol(argv[3]));
    if (mode == "create" && argc == 4) {
        return measure_create(region, records);
    }
    if (argc != 5) {
        return usage();
    }
    const long orders = std::atol(argv[4]);
    if (mode == "fill") {
        return fill(region, records, orders);
    }
    if (mode == "recover") {
        return measure_recover(region, records, orders);
    }
    return usage();
}
