#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

namespace {

constexpr std::size_t small_buffer_size = 256;
constexpr std::size_t large_buffer_size = 4096;

} // namespace

class BumpAllocatorTest : public ::testing::Test {
protected:
    std::array<uint8_t, small_buffer_size> small_buffer_{};
    std::array<uint8_t, large_buffer_size> large_buffer_{};
};

TEST_F(BumpAllocatorTest, ConstructionSucceedsWithValidArguments) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    EXPECT_EQ(allocator.bytes_capacity(), small_buffer_size);
    EXPECT_EQ(allocator.bytes_used(), 0u);
    EXPECT_EQ(allocator.bytes_remaining(), small_buffer_size);
}

TEST_F(BumpAllocatorTest, ConstructionThrowsOnNullStorage) {
    EXPECT_THROW(BumpAllocator(nullptr, small_buffer_size), PreconditionAssertion);
}

TEST_F(BumpAllocatorTest, ConstructionThrowsOnZeroCapacity) {
    EXPECT_THROW(BumpAllocator(small_buffer_.data(), 0), PreconditionAssertion);
}

TEST_F(BumpAllocatorTest, AllocateSingleInt32ReturnsNonNull) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    int32_t* ptr = allocator.allocate<int32_t>(1);
    EXPECT_NE(ptr, nullptr);
}

TEST_F(BumpAllocatorTest, AllocateSingleInt32AdvancesCursor) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(1);
    EXPECT_EQ(allocator.bytes_used(), sizeof(int32_t));
}

TEST_F(BumpAllocatorTest, AllocateMultipleElementsAdvancesCursorCorrectly) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(4);
    EXPECT_EQ(allocator.bytes_used(), sizeof(int32_t) * 4);
}

TEST_F(BumpAllocatorTest, AllocateReturnsPointerInsideBackingBuffer) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    int32_t* ptr = allocator.allocate<int32_t>(1);
    EXPECT_GE(reinterpret_cast<uint8_t*>(ptr), small_buffer_.data());
    EXPECT_LT(reinterpret_cast<uint8_t*>(ptr), small_buffer_.data() + small_buffer_.size());
}

TEST_F(BumpAllocatorTest, AllocateReturnsNullptrWhenCapacityExhausted) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] uint8_t* fill = allocator.allocate<uint8_t>(small_buffer_size);
    uint8_t* ptr = allocator.allocate<uint8_t>(1);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(BumpAllocatorTest, AllocateReturnsNullptrWhenRequestExceedsRemainingCapacity) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] uint8_t* fill = allocator.allocate<uint8_t>(small_buffer_size - 4);
    uint8_t* ptr = allocator.allocate<uint8_t>(8);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(BumpAllocatorTest, AllocateThrowsOnZeroElementCount) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    EXPECT_THROW(
        [&allocator]() { [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(0); }(),
        PreconditionAssertion);
}

TEST_F(BumpAllocatorTest, SuccessiveAllocationsDoNotOverlap) {
    BumpAllocator allocator(large_buffer_.data(), large_buffer_.size());
    int32_t* first = allocator.allocate<int32_t>(4);
    int32_t* second = allocator.allocate<int32_t>(4);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_GE(reinterpret_cast<uint8_t*>(second),
              reinterpret_cast<uint8_t*>(first) + sizeof(int32_t) * 4);
}

TEST_F(BumpAllocatorTest, AllocatedMemoryIsWritable) {
    BumpAllocator allocator(large_buffer_.data(), large_buffer_.size());
    int32_t* ptr = allocator.allocate<int32_t>(3);
    ASSERT_NE(ptr, nullptr);
    ptr[0] = 10;
    ptr[1] = 20;
    ptr[2] = 30;
    EXPECT_EQ(ptr[0], 10);
    EXPECT_EQ(ptr[1], 20);
    EXPECT_EQ(ptr[2], 30);
}

TEST_F(BumpAllocatorTest, ResetRestoresBytesUsedToZero) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(4);
    EXPECT_GT(allocator.bytes_used(), 0u);
    allocator.reset();
    EXPECT_EQ(allocator.bytes_used(), 0u);
}

TEST_F(BumpAllocatorTest, ResetRestoresFullCapacity) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(4);
    allocator.reset();
    EXPECT_EQ(allocator.bytes_remaining(), small_buffer_size);
}

TEST_F(BumpAllocatorTest, AllocationAfterResetSucceeds) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] uint8_t* fill = allocator.allocate<uint8_t>(small_buffer_size);
    allocator.reset();
    uint8_t* ptr = allocator.allocate<uint8_t>(small_buffer_size);
    EXPECT_NE(ptr, nullptr);
}

TEST_F(BumpAllocatorTest, AlignmentIsCorrectForInt64AfterInt8) {
    BumpAllocator allocator(large_buffer_.data(), large_buffer_.size());
    [[maybe_unused]] int8_t* misalign = allocator.allocate<int8_t>(1);
    int64_t* ptr = allocator.allocate<int64_t>(1);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % alignof(int64_t), 0u);
}

TEST_F(BumpAllocatorTest, AlignmentIsCorrectForStringViewAfterInt8) {
    BumpAllocator allocator(large_buffer_.data(), large_buffer_.size());
    [[maybe_unused]] int8_t* misalign = allocator.allocate<int8_t>(1);
    std::string_view* ptr = allocator.allocate<std::string_view>(1);
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % alignof(std::string_view), 0u);
}

TEST_F(BumpAllocatorTest, BytesRemainingDecreasesAfterAllocation) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    std::size_t before = allocator.bytes_remaining();
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(1);
    EXPECT_LT(allocator.bytes_remaining(), before);
}

TEST_F(BumpAllocatorTest, BytesUsedPlusBytesRemainingEqualsCapacity) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    [[maybe_unused]] int32_t* ptr = allocator.allocate<int32_t>(3);
    EXPECT_EQ(allocator.bytes_used() + allocator.bytes_remaining(), allocator.bytes_capacity());
}

TEST_F(BumpAllocatorTest, MultipleResetCyclesWorkCorrectly) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    for (int cycle = 0; cycle < 5; ++cycle) {
        int32_t* ptr = allocator.allocate<int32_t>(4);
        EXPECT_NE(ptr, nullptr);
        allocator.reset();
        EXPECT_EQ(allocator.bytes_used(), 0u);
    }
}

TEST_F(BumpAllocatorTest, ExactCapacityAllocationSucceeds) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    uint8_t* ptr = allocator.allocate<uint8_t>(small_buffer_size);
    EXPECT_NE(ptr, nullptr);
    EXPECT_EQ(allocator.bytes_remaining(), 0u);
}

TEST_F(BumpAllocatorTest, OneByteOverCapacityFails) {
    BumpAllocator allocator(small_buffer_.data(), small_buffer_.size());
    uint8_t* ptr = allocator.allocate<uint8_t>(small_buffer_size + 1);
    EXPECT_EQ(ptr, nullptr);
}

TEST_F(BumpAllocatorTest, UsableAsDecodeArena) {
    // Demonstrates the decode context naming convention.
    // A BumpAllocator named decode_arena is used to allocate view structs
    // during message decoding.
    BumpAllocator decode_arena(large_buffer_.data(), large_buffer_.size());
    std::string_view* views = decode_arena.allocate<std::string_view>(4);
    ASSERT_NE(views, nullptr);
    views[0] = std::string_view("alpha");
    views[1] = std::string_view("beta");
    views[2] = std::string_view("gamma");
    views[3] = std::string_view("delta");
    EXPECT_EQ(views[2], "gamma");
    decode_arena.reset();
    EXPECT_EQ(decode_arena.bytes_used(), 0u);
}

TEST_F(BumpAllocatorTest, UsableAsEncodeArena) {
    // Demonstrates the encode context naming convention.
    // A BumpAllocator named encode_arena is used to allocate ListView data
    // arrays when building a message for encoding.
    BumpAllocator encode_arena(large_buffer_.data(), large_buffer_.size());
    int32_t* values = encode_arena.allocate<int32_t>(3);
    ASSERT_NE(values, nullptr);
    values[0] = 100;
    values[1] = 200;
    values[2] = 300;
    EXPECT_EQ(values[1], 200);
    encode_arena.reset();
    EXPECT_EQ(encode_arena.bytes_used(), 0u);
}

} // namespace pubsub_itc_fw
