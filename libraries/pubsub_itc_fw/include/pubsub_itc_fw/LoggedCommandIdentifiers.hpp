#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/GrowthReportingAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The identities of the commands a WAL holds, each with the position of the command in the WAL, so
 *        that the WAL can tell whether it already holds a command.
 *
 * A command's identity is the bytes CommandIdentityInterface gives for it. Each is held as a 64-bit number
 * made from those bytes (`identifier`). Two different identities can, rarely, give the same number, so a
 * number alone says only that the WAL may hold the command. Beside each number the record keeps the
 * position of the command that gave it, so that the WAL can read that one record and compare the
 * identities exactly (docs/availability/one_check_against_sequencing_twice.md, section 3.2).
 *
 * The record keeps the first position it is given for a number, and a later command with the same number
 * does not change it. So a record at the position that holds a different identity does not mean the WAL
 * lacks the command: two commands may have given the same number, or the WAL may have been cut back and
 * the command written again elsewhere. The WAL then searches for the command instead.
 *
 * **The storage is reserved when the record is created and never grows.** The map is a `tsl::robin_map`,
 * which grows by rehashing every entry in one operation, and a rehash at this size would stall the thread
 * that sequences orders for seconds. So it is reserved for the capacity given, and once it holds that many
 * identities it accepts no more and says so (`Added::full`). Reserving writes to the whole table, so no
 * insert during the day waits for the kernel to supply a page. A slot is 24 bytes: with the capacity the
 * venue uses, 200 million, the table is 2^28 slots, 6 GiB.
 *
 * Every trading day starts with an empty WAL, so the record holds one day at most.
 *
 * One thread calls every member function.
 */
class LoggedCommandIdentifiers {
  public:
    /// How an identity becomes a number. Replaceable only so that a test can make two identities collide.
    using NumberFunction = uint64_t (*)(std::string_view identity);

    // The numbers are already well mixed, so the map uses them as they are.
    struct UseAsHash {
        size_t operator()(uint64_t value) const {
            return static_cast<size_t>(value);
        }
    };
    using Allocator = GrowthReportingAllocator<std::pair<uint64_t, uint64_t>>;
    using Map = tsl::robin_map<uint64_t, uint64_t, UseAsHash, std::equal_to<uint64_t>, Allocator>;

    // Robin Hood hashing stays fast close to full, which is what lets 200 million identities fit in 2^28
    // slots rather than 2^29.
    static constexpr float max_load_factor = 0.9F;

    /// The number of bits of a packed position that hold the offset within a segment; the rest hold the segment.
    static constexpr unsigned offset_bits = 48U;

    /// A packed position that stands for none: a position too large to pack.
    static constexpr uint64_t no_position = ~uint64_t{0};

    /**
     * @param[in] capacity        How many identities to reserve room for; at least one.
     * @param[in] growth_reporter Told of the table's allocation, so that its size appears with the process's
     *                            other large allocations. May be null.
     * @param[in] number_of       How an identity becomes a number.
     */
    LoggedCommandIdentifiers(size_t capacity, AllocationGrowthReporter* growth_reporter, NumberFunction number_of = &identifier)
        : capacity_(capacity), number_of_(number_of), map_(make_map(capacity, growth_reporter)) {}

    /// The 64-bit number that stands for @p identity.
    [[nodiscard]] static uint64_t identifier(std::string_view identity) {
        return mix(std::hash<std::string_view>{}(identity) ^ identity.size());
    }

    /// The number this record uses for @p identity.
    [[nodiscard]] uint64_t number_of(std::string_view identity) const {
        return number_of_(identity);
    }

    /// A position as one 64-bit number, or no_position when it is too large to pack.
    [[nodiscard]] static uint64_t pack(WalPosition position) {
        if (position.segment >= (uint64_t{1} << (64U - offset_bits)) || position.offset >= (uint64_t{1} << offset_bits)) {
            return no_position;
        }
        return (position.segment << offset_bits) | position.offset;
    }

    /// The position a packed number stands for. Not to be called with no_position.
    [[nodiscard]] static WalPosition unpack(uint64_t packed) {
        return WalPosition{packed >> offset_bits, packed & ((uint64_t{1} << offset_bits) - 1)};
    }

    enum class Added { added, already_present, full };

    /// Adds the number @p id, with the position of the command that gave it, unless the number is already
    /// present, when its first position is kept, or the record holds as many as it was reserved for.
    Added add(uint64_t id, WalPosition position) {
        if (map_.size() >= capacity_) {
            return map_.count(id) != 0 ? Added::already_present : Added::full;
        }
        return map_.try_emplace(id, pack(position)).second ? Added::added : Added::already_present;
    }

    /// What the record knows of a number.
    struct Lookup {
        /// False means the WAL certainly does not hold a command with this number, unless the record is full.
        bool may_hold{false};
        /// Whether a position is known for it.
        bool position_known{false};
        /// The position of the first command with this number, when known.
        WalPosition position{};
    };

    [[nodiscard]] Lookup find(uint64_t id) const {
        const auto found = map_.find(id);
        if (found == map_.end()) {
            return Lookup{};
        }
        if (found->second == no_position) {
            return Lookup{true, false, {}};
        }
        return Lookup{true, true, unpack(found->second)};
    }

    [[nodiscard]] size_t size() const {
        return map_.size();
    }

    [[nodiscard]] size_t capacity() const {
        return capacity_;
    }

    /// Whether the record holds as many identities as it was reserved for, and so takes no more.
    [[nodiscard]] bool full() const {
        return map_.size() >= capacity_;
    }

    /// The number of slots in the table, which is what its memory is.
    [[nodiscard]] size_t slot_count() const {
        return map_.bucket_count();
    }

  private:
    // The final mixing step of SplitMix64: spreads every input bit across every output bit, so that the
    // low bits the table uses to pick a slot depend on the whole identity.
    [[nodiscard]] static uint64_t mix(uint64_t value) {
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return value;
    }

    [[nodiscard]] static Map make_map(size_t capacity, AllocationGrowthReporter* growth_reporter) {
        if (capacity == 0) {
            throw PreconditionAssertion("LoggedCommandIdentifiers: the capacity must be at least one", __FILE__, __LINE__);
        }
        Map map(0, UseAsHash{}, std::equal_to<uint64_t>{}, Allocator{growth_reporter});
        map.max_load_factor(max_load_factor);
        map.reserve(capacity);
        return map;
    }

    size_t capacity_;
    NumberFunction number_of_;
    Map map_;
};

} // namespaces
