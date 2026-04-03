#include "DslBenchProtocol.hpp"

#include <chrono>
#include <iostream>
#include <string_view>

using namespace pubsub_itc_fw;

int main() {
    uint8_t buffer[4096];
    std::size_t written = 0;
    std::size_t consumed = 0;

    // ------------------------------------------------------------
    // SmallMessage benchmark
    // ------------------------------------------------------------
    SmallMessage small{};
    small.name = "example-name";
    small.value = 12345;

    auto start = std::chrono::high_resolution_clock::now();
    encode(small, buffer, sizeof(buffer), written);
    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "SmallMessage encode ns: "
              << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
              << "\n";

    SmallMessage small_decoded{};
    start = std::chrono::high_resolution_clock::now();
    decode(small_decoded, buffer, written, consumed);
    end = std::chrono::high_resolution_clock::now();

    std::cout << "SmallMessage decode ns: "
              << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
              << "\n";

    // ------------------------------------------------------------
    // MediumMessage benchmark
    // ------------------------------------------------------------
    MediumMessage medium{};

    static std::string_view tags[] = {
        "alpha", "beta", "gamma", "delta", "epsilon",
        "zeta", "eta", "theta", "iota", "kappa"
    };

    medium.tags.data = tags;
    medium.tags.size = 10;
    medium.sequence = 987654321;

    start = std::chrono::high_resolution_clock::now();
    encode(medium, buffer, sizeof(buffer), written);
    end = std::chrono::high_resolution_clock::now();

    std::cout << "MediumMessage encode ns: "
              << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
              << "\n";

    MediumMessage medium_decoded{};
    start = std::chrono::high_resolution_clock::now();
    decode(medium_decoded, buffer, written, consumed);
    end = std::chrono::high_resolution_clock::now();

    std::cout << "MediumMessage decode ns: "
              << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()
              << "\n";

    return 0;
}
