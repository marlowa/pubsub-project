// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/Crc32.hpp>

using pubsub_itc_fw::Crc32;

// ---------------------------------------------------------------------------
// Known-good test vectors (CRC-32/ISO-HDLC, IEEE polynomial 0xEDB88320)
// ---------------------------------------------------------------------------

TEST(Crc32Test, EmptyInputGivesZero) {
    EXPECT_EQ(Crc32::compute(nullptr, 0), 0x00000000u);
}

TEST(Crc32Test, SingleByteA) {
    const char data = 'a';
    EXPECT_EQ(Crc32::compute(&data, 1), 0xE8B7BE43u);
}

TEST(Crc32Test, ThreeBytesAbc) {
    const char data[] = "abc";
    EXPECT_EQ(Crc32::compute(data, 3), 0x352441C2u);
}

TEST(Crc32Test, StandardCheckVector123456789) {
    // Canonical CRC-32 check value defined in the spec.
    const char data[] = "123456789";
    EXPECT_EQ(Crc32::compute(data, 9), 0xCBF43926u);
}

// ---------------------------------------------------------------------------
// Incremental feed produces same result as single-pass compute
// ---------------------------------------------------------------------------

TEST(Crc32Test, IncrementalFeedMatchesCompute) {
    const char part1[] = "Hello, ";
    const char part2[] = "world!";

    char combined[14];
    std::memcpy(combined, part1, 7);
    std::memcpy(combined + 7, part2, 6);

    Crc32 crc;
    crc.feed(part1, 7);
    crc.feed(part2, 6);
    const uint32_t incremental = crc.finalize();

    const uint32_t single = Crc32::compute(combined, 13);
    EXPECT_EQ(incremental, single);
}

TEST(Crc32Test, ByteByByteMatchesCompute) {
    const std::string data = "123456789";

    Crc32 crc;
    for (char c : data) {
        crc.feed(&c, 1);
    }
    EXPECT_EQ(crc.finalize(), Crc32::compute(data.data(), data.size()));
}

// ---------------------------------------------------------------------------
// finalize() resets state -- can be reused
// ---------------------------------------------------------------------------

TEST(Crc32Test, FinalizeResetsState) {
    const char data[] = "abc";

    Crc32 crc;
    crc.feed(data, 3);
    const uint32_t first = crc.finalize();

    crc.feed(data, 3);
    const uint32_t second = crc.finalize();

    EXPECT_EQ(first, second);
}

TEST(Crc32Test, FinalizeOnFreshObjectGivesZero) {
    Crc32 crc;
    EXPECT_EQ(crc.finalize(), 0x00000000u);
}

// ---------------------------------------------------------------------------
// Order sensitivity -- different orderings give different results
// ---------------------------------------------------------------------------

TEST(Crc32Test, OrderMatters) {
    const char ab[] = "ab";
    const char ba[] = "ba";
    EXPECT_NE(Crc32::compute(ab, 2), Crc32::compute(ba, 2));
}

// ---------------------------------------------------------------------------
// Distinct inputs give distinct checksums
// ---------------------------------------------------------------------------

TEST(Crc32Test, DifferentInputsGiveDifferentChecksums) {
    EXPECT_NE(Crc32::compute("foo", 3), Crc32::compute("bar", 3));
}
