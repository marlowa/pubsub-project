#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "EngineReportPosition.hpp"

namespace sequencer {

/**
 * @brief Copies of the execution reports a sequencer that is not leading receives from the matching
 *        engine, kept so that it can forward them if it takes the lead.
 *
 * The matching engine sends every execution report to both sequencers. Only the leader forwards a
 * report to the member's gateway. If the leader dies, the reports it received but had not yet
 * forwarded would never reach the member, so the other sequencer keeps a copy of each report it
 * receives, and forwards every copy it holds when it takes the lead, each marked as a possible
 * repeat (docs/availability/change_of_sequencer_leader.md, section 4.4).
 *
 * A copy is the report exactly as the matching engine sent it: the bytes of its WalRecord envelope,
 * together with the sequence number the engine sent with it and the time it arrived.
 *
 * **The storage is allocated once, when the store is created.** The copies are written one after
 * another into a block of bytes of fixed size, and when the end of the block is reached the next
 * copy is written at its start, over the oldest copies. A second, fixed-size list records where
 * each copy is, oldest first. Keeping a report therefore allocates nothing.
 *
 * **A copy is discarded in one of two ways.** The sequencer discards copies that have become too
 * old to be needed, by calling discard_received_before. And when there is no room for a new copy,
 * the oldest copies are overwritten to make room. A copy overwritten while it was still young
 * enough to be needed is a report that may now never reach its member, so those are counted, and
 * the sequencer reports the count.
 *
 * One thread, the sequencer's, calls every member function.
 */
class KeptReportStore {
  public:
    using Clock = std::chrono::steady_clock;

    /**
     * @param[in] capacity_bytes The size of the block the copies are written into.
     * @param[in] capacity_reports The most copies held at once, however small they are.
     */
    KeptReportStore(size_t capacity_bytes, size_t capacity_reports) : bytes_(capacity_bytes), entries_(capacity_reports) {
        if (capacity_bytes == 0) {
            throw pubsub_itc_fw::PreconditionAssertion("KeptReportStore: the capacity in bytes must be greater than zero", __FILE__, __LINE__);
        }
    }

    /**
     * @brief Keeps a copy of a report.
     *
     * Copies older than the room needed are overwritten, oldest first. Each one overwritten that
     * arrived at or after @p needed_from is counted as lost.
     *
     * @param[in] seq_no The sequence number the matching engine sent with the report.
     * @param[in] bytes The report's WalRecord envelope, as received.
     * @param[in] size The number of bytes at @p bytes.
     * @param[in] received_at When the report arrived.
     * @param[in] needed_from Copies that arrived at or after this time may still be needed.
     * @param[in] position Where the report stands in the engine's reports, when it says.
     */
    void keep(int64_t seq_no, const uint8_t* bytes, size_t size, Clock::time_point received_at, Clock::time_point needed_from,
              std::optional<EngineReportPosition> position = std::nullopt) {
        if (size > bytes_.size()) {
            // Larger than the whole block, so it cannot be kept however much is overwritten.
            ++lost_;
            return;
        }
        const size_t offset = make_room(size, needed_from);
        std::memcpy(bytes_.data() + offset, bytes, size);
        // make_room has left a free slot in the list, so this cannot be refused.
        (void)entries_.push_back(Entry{offset, size, seq_no, received_at, position});
        next_offset_ = offset + size;
        bytes_used_ += size;
    }

    /**
     * @brief Discards every copy of a report that arrived before @p cutoff.
     *
     * Copies arrive in time order, so this removes copies from the oldest end until it reaches one
     * that arrived at or after @p cutoff.
     */
    void discard_received_before(Clock::time_point cutoff) {
        while (!entries_.empty() && entries_.front().received_at < cutoff) {
            discard_oldest();
        }
    }

    /**
     * @brief Discards every copy of a report at or before @p forwarded_through: the leader has
     *        forwarded every one of those, so none needs forwarding again.
     *
     * Reports arrive in the order the engine numbered them, so this removes copies from the oldest end
     * until it reaches one after @p forwarded_through, or one that does not say where it stands.
     *
     * @return How many copies were discarded.
     */
    size_t discard_through(const EngineReportPosition& forwarded_through) {
        size_t discarded = 0;
        while (!entries_.empty() && entries_.front().position.has_value() && entries_.front().position->is_before_or_at(forwarded_through)) {
            discard_oldest();
            ++discarded;
        }
        return discarded;
    }

    /**
     * @brief Hands every copy to @p visit, oldest first, and then empties the store.
     *
     * @p visit is called as visit(seq_no, bytes, size). The bytes are valid only during the call.
     */
    template <typename Visit> void take_all(Visit&& visit) {
        while (!entries_.empty()) {
            const Entry& oldest = entries_.front();
            visit(oldest.seq_no, bytes_.data() + oldest.offset, oldest.size);
            discard_oldest();
        }
    }

    /// The number of copies held.
    [[nodiscard]] size_t count() const {
        return entries_.size();
    }

    /// The number of bytes the copies held occupy.
    [[nodiscard]] size_t bytes_used() const {
        return bytes_used_;
    }

    /// How many reports have been overwritten, or not kept at all, while they might still have been needed, since the store was created.
    [[nodiscard]] int64_t lost() const {
        return lost_;
    }

  private:
    struct Entry {
        size_t offset{0};
        size_t size{0};
        int64_t seq_no{0};
        Clock::time_point received_at{};
        std::optional<EngineReportPosition> position;
    };

    // Returns the offset at which a copy of @p size bytes can be written, overwriting the oldest
    // copies until there is room for it and a free slot in the list.
    //
    // While nothing has been overwritten at the start of the block, the copies lie in one stretch,
    // from the oldest copy's offset to next_offset_. Once a copy has been written at the start, they
    // lie in two: from the oldest copy's offset to wherever the copies before the wrap ended, and
    // from the start of the block to next_offset_. wrapped_ says which of the two is the case.
    [[nodiscard]] size_t make_room(size_t size, Clock::time_point needed_from) {
        for (;;) {
            if (entries_.empty()) {
                next_offset_ = 0;
                wrapped_ = false;
                return 0;
            }
            if (!entries_.full()) {
                const size_t oldest_offset = entries_.front().offset;
                if (!wrapped_) {
                    if (bytes_.size() - next_offset_ >= size) {
                        return next_offset_;
                    }
                    if (oldest_offset >= size) {
                        wrapped_ = true;
                        return 0;
                    }
                } else if (oldest_offset - next_offset_ >= size) {
                    return next_offset_;
                }
            }
            if (entries_.front().received_at >= needed_from) {
                ++lost_;
            }
            discard_oldest();
        }
    }

    void discard_oldest() {
        const size_t offset = entries_.front().offset;
        bytes_used_ -= entries_.front().size;
        entries_.pop_front();
        if (entries_.empty()) {
            next_offset_ = 0;
            wrapped_ = false;
        } else if (wrapped_ && entries_.front().offset < offset) {
            // The copies before the wrap are all gone; what remains lies in one stretch again.
            wrapped_ = false;
        }
    }

    std::vector<uint8_t> bytes_;
    pubsub_itc_fw::FixedCapacityRingBuffer<Entry> entries_;
    size_t next_offset_{0};
    size_t bytes_used_{0};
    bool wrapped_{false};
    int64_t lost_{0};
};

} // namespaces
