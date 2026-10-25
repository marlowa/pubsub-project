#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include "SessionIdentity.hpp"

namespace sequencer {

/**
 * @brief Execution reports a leading sequencer could not yet send, because the member's session was not known to it,
 *        held until the session is announced.
 *
 * A sequencer keeps where each session is connected only in memory, and learns it from the gateways, which announce
 * every session they hold when their link to the sequencer comes up. Until every gateway has announced, a report for a
 * session the sequencer does not know of may be for a session that is connected after all, and is held here. When the
 * session is announced, its reports are handed back, oldest first, to be sent. When every gateway has announced, the
 * reports still held are for sessions connected nowhere, and are handed back to be dropped. The design is
 * docs/availability/reports_held_until_sessions_are_announced.md.
 *
 * A held report is a record of bytes the sequencer makes and reads back; this class does not look inside it.
 *
 * **The storage is allocated once, when the store is created**, as in KeptReportStore. The records are written one
 * after another into a block of bytes of fixed size, continuing from its start when its end is reached, and a
 * fixed-size list of entries, oldest first, says where each record is and whose session it is for. Holding a report
 * allocates nothing.
 *
 * **Reports for one session can be handed back while older reports for other sessions stay held.** Their entries are
 * marked as handed back and keep their room until every older entry has gone too, so the room of the records still
 * held is always one stretch, or two once it has continued from the start of the block.
 *
 * **When there is no room, the oldest record still held is dropped to make room**, and counted. Holding lasts from
 * taking the lead until the gateways have announced their sessions, normally under a second, so the room is not
 * expected to fill.
 *
 * The sequencer also keeps one of these stores for each configured gateway instance, for the reports it cannot send
 * because it has no connection for reports to that gateway, and hands them all back when the connection is made
 * (BUG-0202).
 *
 * One thread, the sequencer's, calls every member function.
 */
class ReportsHeldForUnknownSessions {
  public:
    /**
     * @param[in] capacity_bytes The size of the block the records are written into.
     * @param[in] capacity_reports The most records held at once, however small they are.
     */
    ReportsHeldForUnknownSessions(size_t capacity_bytes, size_t capacity_reports) : bytes_(capacity_bytes), entries_(capacity_reports) {
        if (capacity_bytes == 0 || capacity_reports == 0) {
            throw pubsub_itc_fw::PreconditionAssertion("ReportsHeldForUnknownSessions: both capacities must be greater than zero", __FILE__, __LINE__);
        }
    }

    /**
     * @brief Holds a copy of the @p size bytes at @p bytes, a report for @p identity.
     *
     * The oldest records still held are dropped, and counted, until there is room. A record larger than the whole
     * block cannot be held at all, and is counted as dropped. @p size must be at least one.
     */
    void hold(const fix_common::SessionIdentity& identity, const uint8_t* bytes, size_t size) {
        if (size == 0) {
            // Every record occupies some room, which is what lets the two stretches be told apart in make_room.
            throw pubsub_itc_fw::PreconditionAssertion("ReportsHeldForUnknownSessions: a record must have at least one byte", __FILE__, __LINE__);
        }
        if (size > bytes_.size()) {
            ++dropped_for_room_;
            return;
        }
        const size_t offset = make_room(size);
        std::memcpy(bytes_.data() + offset, bytes, size);
        Entry& entry = entries_[index_after_oldest(entry_count_)];
        entry = Entry{identity, offset, size, false};
        ++entry_count_;
        ++held_;
        next_offset_ = offset + size;
        bytes_used_ += size;
    }

    /**
     * @brief Hands every record held for @p identity to @p visit, oldest first, and stops holding them.
     *
     * @p visit is called as visit(bytes, size). The bytes are valid only during the call, and @p visit must not call
     * any member function of this store.
     *
     * @return How many records were handed back.
     */
    template <typename Visit> size_t release(const fix_common::SessionIdentity& identity, Visit&& visit) {
        size_t released = 0;
        for (size_t position = 0; position < entry_count_; ++position) {
            Entry& entry = entries_[index_after_oldest(position)];
            if (!entry.handed_back && entry.identity == identity) {
                visit(static_cast<const uint8_t*>(bytes_.data() + entry.offset), entry.size);
                entry.handed_back = true;
                --held_;
                ++released;
            }
        }
        remove_handed_back_oldest();
        return released;
    }

    /**
     * @brief Hands every record still held to @p visit, oldest first, and empties the store.
     *
     * @p visit is called as visit(identity, bytes, size), with the same conditions as for release().
     *
     * @return How many records were handed back.
     */
    template <typename Visit> size_t take_all(Visit&& visit) {
        size_t taken = 0;
        for (size_t position = 0; position < entry_count_; ++position) {
            const Entry& entry = entries_[index_after_oldest(position)];
            if (!entry.handed_back) {
                visit(entry.identity, static_cast<const uint8_t*>(bytes_.data() + entry.offset), entry.size);
                ++taken;
            }
        }
        discard_all();
        return taken;
    }

    /**
     * @brief Shows @p visit every record still held, oldest first, without handing any back.
     *
     * @p visit is called as visit(identity, bytes, size), with the same conditions as for release().
     */
    template <typename Visit> void for_each_held(Visit&& visit) const {
        for (size_t position = 0; position < entry_count_; ++position) {
            const Entry& entry = entries_[index_after_oldest(position)];
            if (!entry.handed_back) {
                visit(entry.identity, static_cast<const uint8_t*>(bytes_.data() + entry.offset), entry.size);
            }
        }
    }

    /// Stops holding every record, without handing any back.
    void discard_all() {
        oldest_ = 0;
        entry_count_ = 0;
        held_ = 0;
        next_offset_ = 0;
        bytes_used_ = 0;
        wrapped_ = false;
    }

    /// How many records are held.
    [[nodiscard]] size_t count() const {
        return held_;
    }

    /// How many bytes the entries still in the list occupy, those handed back but not yet removed included.
    [[nodiscard]] size_t bytes_used() const {
        return bytes_used_;
    }

    /// How many records have been dropped to make room, or because they could never fit, since the store was created.
    [[nodiscard]] int64_t dropped_for_room() const {
        return dropped_for_room_;
    }

  private:
    struct Entry {
        fix_common::SessionIdentity identity{};
        size_t offset{0};
        size_t size{0};
        bool handed_back{false};
    };

    // The index in entries_ of the entry @p position places after the oldest.
    [[nodiscard]] size_t index_after_oldest(size_t position) const {
        size_t index = oldest_ + position;
        if (index >= entries_.size()) {
            index -= entries_.size();
        }
        return index;
    }

    // Returns the offset at which a record of @p size bytes can be written, removing the oldest entries until there is
    // room for it and a free entry.
    //
    // While nothing has been written at the start of the block since it was last empty, the entries' records lie in one
    // stretch, from the oldest entry's offset to next_offset_. Once a record has been written at the start, they lie in
    // two: from the oldest entry's offset to wherever the records before that ended, and from the start of the block to
    // next_offset_. wrapped_ says which of the two is the case.
    [[nodiscard]] size_t make_room(size_t size) {
        for (;;) {
            if (entry_count_ == 0) {
                next_offset_ = 0;
                wrapped_ = false;
                return 0;
            }
            if (entry_count_ < entries_.size()) {
                const size_t oldest_offset = entries_[oldest_].offset;
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
            if (!entries_[oldest_].handed_back) {
                ++dropped_for_room_;
                --held_;
            }
            remove_oldest();
        }
    }

    // Removes the oldest entries for as long as they have been handed back, which frees their room.
    void remove_handed_back_oldest() {
        while (entry_count_ > 0 && entries_[oldest_].handed_back) {
            remove_oldest();
        }
    }

    void remove_oldest() {
        const size_t offset = entries_[oldest_].offset;
        bytes_used_ -= entries_[oldest_].size;
        oldest_ = index_after_oldest(1);
        --entry_count_;
        if (entry_count_ == 0) {
            oldest_ = 0;
            next_offset_ = 0;
            wrapped_ = false;
        } else if (wrapped_ && entries_[oldest_].offset < offset) {
            // The records before the block continued from its start are all gone; what remains lies in one stretch again.
            wrapped_ = false;
        }
    }

    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    size_t oldest_{0};
    size_t entry_count_{0};
    size_t held_{0};
    size_t next_offset_{0};
    size_t bytes_used_{0};
    bool wrapped_{false};
    int64_t dropped_for_room_{0};
};

} // namespaces
