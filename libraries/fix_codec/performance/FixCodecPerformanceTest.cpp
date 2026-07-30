// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * Performance regression guard for the FIX codec hot path.
 *
 * This is deliberately not part of fix_codec_tests. Unit tests must be fast and
 * deterministic; a timing test is neither, and mixing them would make an ordinary
 * unit-test failure ambiguous. It is a separate binary that build.py runs after the
 * unit and integration suites; ./build.sh --no-performance-tests skips just this one.
 *
 * The primary assertions are ratios against the cost of framing the same message,
 * not absolute nanosecond ceilings. A ratio cancels out processor speed, frequency
 * scaling and build machine, so the test means the same thing on a developer laptop
 * and on a build agent. It is aimed at the regression that actually happened here:
 * a quadratic scan in the validator that a nanosecond threshold would have caught
 * only on one machine, but which visibly inflates the validate-to-frame ratio
 * everywhere. An absolute ceiling is kept as a loose backstop for gross slowdowns.
 *
 * Built at the project's -O2, never -O3, so it measures what actually ships.
 */

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include <fmt/format.h>
#include <vector>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/FixMessageValidator.hpp>
#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/FixReject.hpp>
#include <fix_codec/fix_dictionary.hpp>

namespace tag = fix_codec::tag;

namespace {

uint64_t sink = 0;

// Ratios of each stage to the cost of framing one message. Measured at roughly
// 4.6 (parse) and 10.3 (parse+validate); the headroom absorbs machine variation
// while still catching a regression that changes the shape of the work.
constexpr double maximum_parse_to_frame_ratio = 9.0;
constexpr double maximum_validate_to_frame_ratio = 18.0;

// Loose backstop for a gross slowdown. Generous enough that ordinary hardware
// differences do not trip it; raise it only with a measurement that justifies it.
constexpr double maximum_parse_and_validate_nanoseconds = 1500.0;

// Eight times the fields should cost around eight times as much, not sixty-four. The
// threshold sits well clear of both: the linear implementation measures near 6 (fixed
// per-message costs do not scale, so it comes in under 8), and the quadratic one it
// replaced measures well above 20.
constexpr double maximum_scaling_ratio = 12.0;

/**
 * @brief Pins the calling thread to the processor it is already running on.
 *
 * On a hybrid processor a thread migrated between a performance and an efficiency
 * core changes speed by more than any regression this test looks for. Pinning to
 * the current processor rather than a fixed index keeps parallel CTest runs from
 * all crowding onto the same core.
 */
void pin_to_current_processor() {
    const int processor = sched_getcpu();
    if (processor < 0) {
        return;
    }
    cpu_set_t processor_set;
    CPU_ZERO(&processor_set);
    CPU_SET(processor, &processor_set);
    sched_setaffinity(0, sizeof(processor_set), &processor_set);
}

/** @brief Mean nanoseconds per iteration of the fastest batch. See FixCodecBenchMain. */
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

// The tags build_new_order_single already writes. Padding must avoid them or the
// message is rejected for a duplicate tag rather than timed.
constexpr int base_message_tags[] = {tag::BeginString, tag::BodyLength, tag::Checksum, tag::MsgType, tag::SenderCompID, tag::TargetCompID, tag::MsgSeqNum,
                                     tag::SendingTime, tag::ClOrdID,    tag::Side,     tag::OrdType, tag::TransactTime, tag::OrderQty,     tag::Price};

[[nodiscard]] bool is_base_message_tag(int candidate) {
    for (const int used : base_message_tags) {
        if (used == candidate) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Tags a NewOrderSingle may carry that take free text, in dictionary order.
 *
 * Padding a message out to a chosen field count needs tags the validator will accept
 * without contrivance: permitted for this message type, plain string format (so any
 * short text is well formed), no enumerated value set to satisfy, not a repeating
 * group counter, not a length tag governing a following data field, and not already
 * written by the base message. The dictionary offers over a thousand, which is ample.
 */
const std::vector<int>& paddable_tags() {
    static const std::vector<int> tags = [] {
        std::vector<int> collected;
        for (int candidate = 1; candidate < 60000; ++candidate) {
            const int index = fix_codec::field_index(candidate);
            if (index < 0 || !fix_codec::is_tag_permitted(fix_codec::msg_type::NewOrderSingle, candidate)) {
                continue;
            }
            if (fix_codec::field_format_at(index) != fix_codec::field_format::fix_string) {
                continue;
            }
            if (fix_codec::has_enum_values_at(index) || fix_codec::group_index_for_counter(candidate) >= 0) {
                continue;
            }
            if (fix_codec::is_data_length_tag(candidate) || is_base_message_tag(candidate)) {
                continue;
            }
            collected.push_back(candidate);
        }
        return collected;
    }();
    return tags;
}

/**
 * @brief Builds a valid NewOrderSingle padded to @p field_count top-level fields.
 *
 * Field count is the lever the scaling test needs: a cost that grows with the square
 * of it, rather than in step with it, is the regression being guarded against.
 */
std::string_view build_padded_new_order_single(char* buffer, size_t capacity, size_t field_count) {
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

    const std::vector<int>& padding = paddable_tags();
    for (size_t added = 0; added < field_count && added < padding.size(); ++added) {
        writer.push_back_field(padding[added], std::string_view("PAD"));
    }
    return writer.finish();
}

class FixCodecPerformanceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        pin_to_current_processor();
        wire_ = build_new_order_single(buffer_, sizeof(buffer_));
        ASSERT_FALSE(wire_.empty()) << "could not build the NewOrderSingle under test";

        const fix_codec::FixMessageReader reader(wire_);
        ASSERT_TRUE(reader.is_valid()) << "the message under test must frame cleanly";
        ASSERT_TRUE(fix_codec::FixMessageValidator(reader).validate().ok())
            << "the message under test must validate cleanly, or the timings measure an early reject rather than a full walk";

        frame_nanoseconds_ = measure_batched_ns(
            [this] {
                fix_codec::FixMessageReader reader(wire_);
                sink += static_cast<uint64_t>(reader.status());
            },
            batch_size, batch_count);
        ASSERT_GT(frame_nanoseconds_, 0.0) << "clock resolution too coarse to measure framing";
    }

    static constexpr int batch_size = 20000;
    static constexpr int batch_count = 100;

    // Field counts for the scaling test. Large enough that a quadratic term dominates
    // if one exists, small enough that the messages stay realistic FIX rather than
    // pathological. Fewer batches: each iteration does far more work.
    static constexpr size_t small_field_count = 100;
    static constexpr size_t large_field_count = 800;
    static constexpr size_t padded_buffer_capacity = 64 * 1024;
    static constexpr int scaling_batch_size = 2000;
    static constexpr int scaling_batch_count = 50;

    char buffer_[256]{};
    std::string_view wire_{};
    double frame_nanoseconds_{0.0};
};

TEST_F(FixCodecPerformanceTest, ParsingStaysProportionateToFraming) {
    const double parse_nanoseconds = measure_batched_ns(
        [this] {
            fix_codec::FixMessageReader reader(wire_);
            sink += static_cast<uint64_t>(reader.status());
            sink += reader.find(tag::ClOrdID).as_string_view().size();
            sink += static_cast<uint64_t>(reader.find(tag::Side).as_char());
        },
        batch_size, batch_count);

    const double ratio = parse_nanoseconds / frame_nanoseconds_;
    EXPECT_LT(ratio, maximum_parse_to_frame_ratio) << "parse " << parse_nanoseconds << " ns is " << ratio << "x framing (" << frame_nanoseconds_
                                                   << " ns), above the permitted " << maximum_parse_to_frame_ratio << "x";
}

TEST_F(FixCodecPerformanceTest, ValidationStaysProportionateToFraming) {
    const double validate_nanoseconds = measure_batched_ns(
        [this] {
            fix_codec::FixMessageReader reader(wire_);
            const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
            sink += static_cast<uint64_t>(reject.reason);
        },
        batch_size, batch_count);

    const double ratio = validate_nanoseconds / frame_nanoseconds_;
    EXPECT_LT(ratio, maximum_validate_to_frame_ratio) << "parse+validate " << validate_nanoseconds << " ns is " << ratio << "x framing (" << frame_nanoseconds_
                                                      << " ns), above the permitted " << maximum_validate_to_frame_ratio
                                                      << "x -- a scan that grows with the field count is the usual cause";
}

TEST_F(FixCodecPerformanceTest, ParseAndValidateStaysUnderTheAbsoluteCeiling) {
    const double validate_nanoseconds = measure_batched_ns(
        [this] {
            fix_codec::FixMessageReader reader(wire_);
            const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
            sink += static_cast<uint64_t>(reject.reason);
        },
        batch_size, batch_count);

    EXPECT_LT(validate_nanoseconds, maximum_parse_and_validate_nanoseconds)
        << "parse+validate " << validate_nanoseconds << " ns exceeds the " << maximum_parse_and_validate_nanoseconds
        << " ns backstop; if this machine is simply slower, confirm with the ratio tests before raising it";
}

/*
 * The regression this guards against is a per-field cost that grows with the number
 * of fields already seen -- the quadratic scan the validator used to do. On a typical
 * order of a dozen or so fields that costs only a few percent and hides comfortably
 * inside machine noise, so it is only visible by changing the field count and watching
 * how the cost responds. Doubling the fields should roughly double the work; a
 * quadratic term drives the ratio towards four instead.
 */
TEST_F(FixCodecPerformanceTest, ValidationCostGrowsInStepWithFieldCount) {
    ASSERT_GE(paddable_tags().size(), large_field_count) << "dictionary does not offer enough paddable tags for the scaling test";

    std::vector<char> small_buffer(padded_buffer_capacity);
    std::vector<char> large_buffer(padded_buffer_capacity);
    const std::string_view small = build_padded_new_order_single(small_buffer.data(), small_buffer.size(), small_field_count);
    const std::string_view large = build_padded_new_order_single(large_buffer.data(), large_buffer.size(), large_field_count);
    ASSERT_FALSE(small.empty());
    ASSERT_FALSE(large.empty());

    const fix_codec::FixMessageReader small_reader(small);
    const fix_codec::FixMessageReader large_reader(large);
    ASSERT_TRUE(fix_codec::FixMessageValidator(small_reader).validate().ok()) << "padded message must validate, or the timing measures an early reject";
    ASSERT_TRUE(fix_codec::FixMessageValidator(large_reader).validate().ok()) << "padded message must validate, or the timing measures an early reject";

    const auto validate_message = [](std::string_view wire) {
        return [wire] {
            fix_codec::FixMessageReader reader(wire);
            const fix_codec::FixReject reject = fix_codec::FixMessageValidator(reader).validate();
            sink += static_cast<uint64_t>(reject.reason);
        };
    };

    const double small_nanoseconds = measure_batched_ns(validate_message(small), scaling_batch_size, scaling_batch_count);
    const double large_nanoseconds = measure_batched_ns(validate_message(large), scaling_batch_size, scaling_batch_count);
    ASSERT_GT(small_nanoseconds, 0.0);

    const double field_ratio = static_cast<double>(large_field_count) / static_cast<double>(small_field_count);
    const double cost_ratio = large_nanoseconds / small_nanoseconds;

    // Logged on success too: the trend over time is the useful signal, and a number
    // creeping towards the threshold is worth seeing before it trips.
    fmt::print("[ SCALING  ] {} fields {:.0f} ns, {} fields {:.0f} ns -- {:.2f}x cost for {:.0f}x the fields (limit {:.1f}x)\n", small_field_count,
               small_nanoseconds, large_field_count, large_nanoseconds, cost_ratio, field_ratio, maximum_scaling_ratio);

    EXPECT_LT(cost_ratio, maximum_scaling_ratio) << "validating " << large_field_count << " fields cost " << cost_ratio << "x validating " << small_field_count
                                                 << " (" << large_nanoseconds << " ns vs " << small_nanoseconds << " ns) for " << field_ratio
                                                 << "x the fields -- a per-field cost that grows with the fields already seen is the usual cause";
}

} // namespaces
