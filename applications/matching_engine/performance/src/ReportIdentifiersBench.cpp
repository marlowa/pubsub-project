// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ReportIdentifiersBench.cpp
 * @brief What making one ExecID costs the matching engine's thread: FF1 over AES-256, as ReportIdentifiers does it.
 *
 *   report_identifiers_bench [identifiers]
 *
 * The engine makes one ExecID for every report, and an OrderID for every order it accepts, on its own thread, so the
 * cost is part of every order's round trip. This makes the given number of ExecIDs, 10 million by default, for
 * consecutive sequence numbers, as the engine does, and prints the time each took on average, and the time of the
 * slowest one in a thousand and in a million. Run it pinned to one core with the machine set up for measuring
 * (scripts/measurement_machine.py), as for any figure that is to be quoted.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <fmt/format.h>

#include "ReportIdentifiers.hpp"

int main(int argc, char** argv) {
    const long count = argc > 1 ? std::atol(argv[1]) : 10'000'000;
    if (count < 1'000'000) {
        fmt::print(stderr, "usage: report_identifiers_bench [identifiers, at least 1000000]\n");
        return 2;
    }
    matching_engine::ReportIdentifiers::Secret secret{};
    for (size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<uint8_t>(index * 13 + 1);
    }
    matching_engine::ReportIdentifiers identifiers(secret);
    identifiers.use_trading_date(20261025);

    using Clock = std::chrono::steady_clock;
    std::vector<int64_t> each(static_cast<size_t>(count));
    matching_engine::ReportIdentifiers::Buffer buffer{};
    // Summed so that the compiler cannot drop the work.
    uint64_t checksum = 0;
    const auto start = Clock::now();
    for (long n = 0; n < count; ++n) {
        const auto before = Clock::now();
        const std::string_view exec_id = identifiers.exec_id(buffer, n + 1, 0);
        const auto after = Clock::now();
        checksum += static_cast<uint8_t>(exec_id[16]);
        each[static_cast<size_t>(n)] = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    }
    const double total_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    std::sort(each.begin(), each.end());
    fmt::print("report_identifiers_bench: {} ExecIDs, {:.0f} ns each on average including the timing; median {} ns, 99.9th percentile {} ns, "
               "99.9999th percentile {} ns, slowest {} ns (checksum {})\n",
               count, total_ns / static_cast<double>(count), each[each.size() / 2], each[each.size() * 999 / 1000], each[each.size() - each.size() / 1'000'000],
               each.back(), checksum);
    return 0;
}
