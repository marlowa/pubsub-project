// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

namespace {

class MirroredBufferTest : public ::testing::Test {
protected:
    const int64_t test_capacity = 65536; // 64KB (multiple of most page sizes)
};

} // namespace

TEST_F(MirroredBufferTest, InitialisesWithExpectedCapacity) {
    const MirroredBuffer buffer(test_capacity);

    EXPECT_GE(buffer.capacity(), test_capacity);
    EXPECT_EQ(buffer.bytes_available(), 0);
    EXPECT_EQ(buffer.space_remaining(), buffer.capacity() - 1);
}

TEST_F(MirroredBufferTest, HandlesSimpleSequentialAccess) {
    MirroredBuffer buffer(test_capacity);
    const std::string data = "The quick brown fox jumps over the lazy dog";
    const auto len = static_cast<int64_t>(data.size());

    std::memcpy(buffer.write_ptr(), data.data(), static_cast<size_t>(len));
    buffer.advance_head(len);

    EXPECT_EQ(buffer.bytes_available(), len);
    EXPECT_EQ(std::memcmp(buffer.read_ptr(), data.data(), static_cast<size_t>(len)), 0);

    buffer.advance_tail(len);
    EXPECT_EQ(buffer.bytes_available(), 0);
}

TEST_F(MirroredBufferTest, VerifiesVirtualMemoryMirroringContinuity) {
    MirroredBuffer buffer(test_capacity);
    const int64_t cap = buffer.capacity();

    // Advance pointers to exactly 10 bytes before the physical wrap point
    const int64_t offset = cap - 10;
    buffer.advance_head(offset);
    buffer.advance_tail(offset);

    // Write a 20-byte string that physically straddles the end of the buffer
    const char* straddle_data = "0123456789ABCDEFGHIJ";
    std::memcpy(buffer.write_ptr(), straddle_data, 20);

    // Adversarial Check: Verify the bytes are physically present at the start of the buffer
    // through the second mapping.
    buffer.advance_head(20);

    // 1. Check contiguous read pointer visibility
    EXPECT_EQ(std::memcmp(buffer.read_ptr(), straddle_data, 20), 0);

    // 2. Check that writing to the 'mirror' at the end of the 2*cap range
    // actually updated the physical memory at the beginning.
    const uint8_t* physical_start = buffer.read_ptr() - offset; // This points to the base_ptr_
    EXPECT_EQ(std::memcmp(physical_start, "ABCDEFGHIJ", 10), 0);
}

TEST_F(MirroredBufferTest, EnforcesOneByteGapFullBufferLogic) {
    MirroredBuffer buffer(test_capacity);
    const int64_t cap = buffer.capacity();

    // Fill the buffer to the maximum allowed limit
    const int64_t max_fill = cap - 1;
    buffer.advance_head(max_fill);

    EXPECT_EQ(buffer.space_remaining(), 0);
    EXPECT_EQ(buffer.bytes_available(), max_fill);

    // Adversarial: Attempting to write even one more byte must fail
    EXPECT_THROW(buffer.advance_head(1), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, ThrowsOnInvalidRequestedCapacity) {
    // Adversarial: Negative or zero capacity
    EXPECT_THROW(MirroredBuffer buffer(-1), PreconditionAssertion);
    EXPECT_THROW(MirroredBuffer buffer(0), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, ThrowsOnInvalidPointerAdvancement) {
    MirroredBuffer buffer(test_capacity);

    // Adversarial: Advance head more than space remaining
    EXPECT_THROW(buffer.advance_head(buffer.capacity()), PreconditionAssertion);

    // Adversarial: Advance tail more than bytes available
    buffer.advance_head(100);
    EXPECT_THROW(buffer.advance_tail(101), PreconditionAssertion);

    // Adversarial: Negative advancement
    EXPECT_THROW(buffer.advance_head(-1), PreconditionAssertion);
    EXPECT_THROW(buffer.advance_tail(-1), PreconditionAssertion);
}

TEST_F(MirroredBufferTest, HandlesExhaustiveWrapAroundStress) {
    MirroredBuffer buffer(test_capacity);
    const int64_t chunk_size = 1000;
    const int64_t iterations = 1000;

    for (int64_t i = 0; i < iterations; ++i) {
        std::memset(buffer.write_ptr(), static_cast<int>(i % 255), static_cast<size_t>(chunk_size));
        buffer.advance_head(chunk_size);

        EXPECT_EQ(*buffer.read_ptr(), static_cast<uint8_t>(i % 255));

        buffer.advance_tail(chunk_size);
        EXPECT_EQ(buffer.bytes_available(), 0);
    }
}

TEST_F(MirroredBufferTest, SupportsLargeAddressSpaceReservations) {
    // Verify 5GB cushion (requires 10GB virtual address space)
    const int64_t five_gb = 5LL * 1024 * 1024 * 1024;

    // This is an adversarial environment test; it may fail on systems with
    // restricted virtual memory (RLIMIT_AS), so we catch the framework exception.
    try {
        const MirroredBuffer massive_buffer(five_gb);
        EXPECT_GE(massive_buffer.capacity(), five_gb);
    } catch (const PubSubItcException& e) {
        // If the OS refuses the 10GB mmap, we log but don't fail the logic test
        SUCCEED() << "System-level restriction on 10GB VMA, exception caught as expected.";
    }
}

} // namespace pubsub_itc_fw
