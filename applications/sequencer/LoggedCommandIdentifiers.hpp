#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <tsl/robin_set.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/GrowthReportingAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace sequencer {

/**
 * @brief The identifiers of the commands a sequencer's log holds, so that a command a gateway sends
 *        again after a change of leader can be recognised if the log already holds it.
 *
 * A command is identified by the session that sent it, its comp id and gateway protocol, and its
 * `ClOrdID`. Each identifier is held as a 64-bit number derived from those three values
 * (`identifier`). A string of dozens of characters cannot be given a different 64-bit number from
 * every other, so two different commands can, rarely, give the same number. The record therefore
 * answers only "possibly held" or "certainly not held":
 *
 * - A number that is not in the record means the log certainly does not hold the command.
 * - A number that is in it means the log may hold it, and the caller checks exactly, by searching
 *   the log for a record with the same three values
 *   (docs/availability/commands_during_a_change_of_leader.md, section 7, decision 1).
 *
 * **The storage is reserved when the record is created and never grows.** The set is a
 * `tsl::robin_set`, which grows by rehashing every entry in one operation, and a rehash at this size
 * would stall the thread that sequences orders for seconds. So it is reserved for the capacity given,
 * and once it holds that many identifiers it accepts no more and says so (`Added::full`). Reserving
 * writes to the whole table, so no insert during the day waits for the kernel to supply a page.
 * With the capacity the venue uses, 200 million, the table is 2^28 slots of 16 bytes, 4 GiB.
 *
 * Every trading day starts with an empty log, so the record holds one day at most.
 *
 * One thread, the sequencer's, calls every member function.
 */
class LoggedCommandIdentifiers {
  public:
    // The numbers are already well mixed, so the set uses them as they are.
    struct UseAsHash {
        size_t operator()(uint64_t value) const {
            return static_cast<size_t>(value);
        }
    };
    using Allocator = pubsub_itc_fw::GrowthReportingAllocator<uint64_t>;
    using Set = tsl::robin_set<uint64_t, UseAsHash, std::equal_to<uint64_t>, Allocator>;

    // Robin Hood hashing stays fast close to full, which is what lets 200 million identifiers fit in
    // 2^28 slots rather than 2^29.
    static constexpr float max_load_factor = 0.9F;

    /**
     * @param[in] capacity How many identifiers to reserve room for; at least one.
     * @param[in] growth_reporter Told of the table's allocation, so that its size appears with the
     *            process's other large allocations. May be null.
     */
    LoggedCommandIdentifiers(size_t capacity, pubsub_itc_fw::AllocationGrowthReporter* growth_reporter)
        : capacity_(capacity), set_(make_set(capacity, growth_reporter)) {}

    /// The 64-bit number that stands for the command @p cl_ord_id sent by the session (@p comp_id, @p protocol).
    [[nodiscard]] static uint64_t identifier(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id) {
        uint64_t value = mix(std::hash<std::string_view>{}(comp_id));
        value = mix(value ^ static_cast<uint64_t>(static_cast<uint16_t>(protocol)));
        value = mix(value ^ std::hash<std::string_view>{}(cl_ord_id));
        return value;
    }

    enum class Added { added, already_present, full };

    /// Adds @p id, unless it is already present or the record holds as many identifiers as it was reserved for.
    Added add(uint64_t id) {
        if (set_.size() >= capacity_) {
            return set_.count(id) != 0 ? Added::already_present : Added::full;
        }
        return set_.insert(id).second ? Added::added : Added::already_present;
    }

    /// Whether the log may hold the command @p id stands for. False means it certainly does not.
    [[nodiscard]] bool may_hold(uint64_t id) const {
        return set_.count(id) != 0;
    }

    [[nodiscard]] size_t size() const {
        return set_.size();
    }

    [[nodiscard]] size_t capacity() const {
        return capacity_;
    }

    /// Whether the record holds as many identifiers as it was reserved for, and so takes no more.
    [[nodiscard]] bool full() const {
        return set_.size() >= capacity_;
    }

    /// The number of slots in the table, which is what its memory is: 16 bytes each.
    [[nodiscard]] size_t slot_count() const {
        return set_.bucket_count();
    }

  private:
    // The final mixing step of SplitMix64: spreads every input bit across every output bit, so that
    // the low bits the table uses to pick a slot depend on the whole identifier.
    [[nodiscard]] static uint64_t mix(uint64_t value) {
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return value;
    }

    [[nodiscard]] static Set make_set(size_t capacity, pubsub_itc_fw::AllocationGrowthReporter* growth_reporter) {
        if (capacity == 0) {
            throw pubsub_itc_fw::PreconditionAssertion("LoggedCommandIdentifiers: the capacity must be at least one", __FILE__, __LINE__);
        }
        Set set(0, UseAsHash{}, std::equal_to<uint64_t>{}, Allocator{growth_reporter});
        set.max_load_factor(max_load_factor);
        set.reserve(capacity);
        return set;
    }

    size_t capacity_;
    Set set_;
};

} // namespaces
