#include "DslBenchProtocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>

using namespace pubsub_itc_fw;

// ------------------------------------------------------------
// Timing helper
// ------------------------------------------------------------
template<typename F>
long long measure_avg_ns(F&& fn, int iterations, long long& min_ns, long long& max_ns)
{
    for (int i = 0; i < 100; ++i)
        fn();

    min_ns = std::numeric_limits<long long>::max();
    max_ns = 0;
    long long total = 0;

    for (int i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto end = std::chrono::high_resolution_clock::now();

        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        total += ns;
        min_ns = std::min(min_ns, ns);
        max_ns = std::max(max_ns, ns);
    }

    return total / iterations;
}

// ------------------------------------------------------------
// Benchmark runner for a single encode/decode pair
// ------------------------------------------------------------
template<typename OwningMsg, typename ViewMsg>
void benchmark_message(const char* name, OwningMsg& msg, int iterations)
{
    uint8_t buffer[65536];
    std::size_t written = 0;
    std::size_t consumed = 0;
    std::size_t bytes_needed = 0;
    ViewMsg decoded{};

    alignas(64) std::array<uint8_t, 4096> decode_arena_storage{};
    BumpAllocator decode_arena(decode_arena_storage.data(), decode_arena_storage.size());

    long long min_enc, max_enc;
    long long min_dec, max_dec;

    auto avg_enc = measure_avg_ns([&] {
        static_cast<void>(encode(msg, buffer, sizeof(buffer), written, bytes_needed));
    }, iterations, min_enc, max_enc);

    auto avg_dec = measure_avg_ns([&] {
        decode_arena.reset();
        decoded = ViewMsg{};
        static_cast<void>(decode(decoded, buffer, written, consumed, decode_arena));
    }, iterations, min_dec, max_dec);

    std::cout << "------------------------------------------------------------\n";
    std::cout << name << " (" << iterations << " iterations)\n";
    std::cout << "  Encode avg: " << avg_enc << " ns  [min=" << min_enc << ", max=" << max_enc << "]\n";
    std::cout << "  Decode avg: " << avg_dec << " ns  [min=" << min_dec << ", max=" << max_dec << "]\n";
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main()
{
    const int iterations = 2000;

    // ------------------------------------------------------------
    // SmallMessage
    // ------------------------------------------------------------
    SmallMessage small{};
    small.name = "example-name";
    small.value = 12345;

    benchmark_message<SmallMessage, SmallMessageView>("SmallMessage", small, iterations);

    // ------------------------------------------------------------
    // MediumMessage
    // ------------------------------------------------------------
    MediumMessage medium{};

    static std::string_view tags[] = {
        "alpha", "beta", "gamma", "delta", "epsilon",
        "zeta", "eta", "theta", "iota", "kappa"
    };

    medium.tags.data = tags;
    medium.tags.size = 10;
    medium.sequence = 987654321;

    benchmark_message<MediumMessage, MediumMessageView>("MediumMessage", medium, iterations);

    // ------------------------------------------------------------
    // LargeMessage (list<list<string>>)
    // ------------------------------------------------------------
    LargeMessage large{};

    static std::string_view group1[] = {"a", "b", "c"};
    static std::string_view group2[] = {"d", "e", "f", "g"};
    static std::string_view group3[] = {"h"};

    static ListView<std::string_view> groups[] = {
        { group1, 3 },
        { group2, 4 },
        { group3, 1 }
    };

    large.groups.data = groups;
    large.groups.size = 3;
    large.sequence = 123456789;

    benchmark_message<LargeMessage, LargeMessageView>("LargeMessage", large, iterations);

    return 0;
}
