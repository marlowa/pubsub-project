#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace fix_common {

/**
 * @brief The commands a gateway has sent to the sequencers and not yet seen answered, kept so that it
 *        can send them again to a new leader after a change of sequencer leader.
 *
 * A gateway sends each command, a new order or a request to cancel, to both sequencers. If the leading
 * sequencer dies, a command can be lost: sent while no instance was leading, or held by the old leader
 * alone. The gateway keeps a copy of each command until the first report for it arrives, and when a
 * new leader announces itself it sends every copy it still holds again, in the order they were first
 * sent (docs/availability/commands_during_a_change_of_leader.md, sections 3.1 and 3.2).
 *
 * A command is identified by the member's comp id and the command's `ClOrdID`. A copy is the encoded
 * `WalRecord` envelope exactly as sent, so that sending it again sends the same command.
 *
 * **The storage is allocated once, when the store is created.** The copies are written one after
 * another into a block of bytes of fixed size, continuing at the start when the end is reached. A list
 * of fixed length records where each copy lies, oldest first, and an index of fixed size finds a copy
 * by its comp id and `ClOrdID`. Keeping and answering a command allocate nothing.
 *
 * **Copies are answered out of order, and their space is reclaimed in order.** An answered copy is
 * marked, and its space is reused once every copy older than it has been answered too. A command that
 * stays unanswered for a long time therefore holds the space behind it, and the store fills. That is
 * the intended behaviour: a venue that is not answering commands should stop taking more, and a full
 * store makes the gateway refuse each new command with a reply.
 *
 * One thread, the gateway's, calls every member function.
 */
class UnansweredCommandStore {
  public:
    /**
     * @param[in] capacity_bytes The size of the block the copies are written into.
     * @param[in] capacity_commands The most copies held at once, answered or not, however small they are.
     */
    UnansweredCommandStore(size_t capacity_bytes, size_t capacity_commands)
        : bytes_(capacity_bytes), entries_(capacity_commands), index_(make_index(capacity_bytes, capacity_commands)) {}

    enum class Kept {
        // A copy is kept until the command is answered.
        kept,
        // The store has no room. The caller refuses the command.
        full,
        // A command with the same comp id and ClOrdID is already held unanswered: a member resubmitting
        // an order it had no answer for. It is not kept a second time. Sending the first copy again is
        // enough, and the venue refuses the repeat as a duplicate.
        already_held
    };

    /// Keeps a copy of a command about to be sent: the member's comp id, the command's ClOrdID, and its encoded envelope.
    Kept keep(std::string_view comp_id, std::string_view cl_ord_id, const uint8_t* envelope, size_t size) {
        const size_t key_size = comp_id.size() + 1 + cl_ord_id.size();
        const size_t total = key_size + size;
        // The key is built in place in the block, so the lookup below needs the bytes written first.
        if (total > bytes_.size()) {
            return Kept::full;
        }
        reclaim_answered();
        if (live_ == entries_.size()) {
            return Kept::full;
        }
        const size_t offset = find_room(total);
        if (offset == no_room) {
            return Kept::full;
        }
        uint8_t* key = bytes_.data() + offset;
        std::memcpy(key, comp_id.data(), comp_id.size());
        key[comp_id.size()] = separator;
        std::memcpy(key + comp_id.size() + 1, cl_ord_id.data(), cl_ord_id.size());
        const std::string_view key_view(reinterpret_cast<const char*>(key), key_size);
        if (index_.find(key_view) != index_.end()) {
            return Kept::already_held;
        }
        std::memcpy(key + key_size, envelope, size);

        const size_t slot = (head_ + live_) % entries_.size();
        entries_[slot] = Entry{offset, key_size, size, false};
        ++live_;
        ++unanswered_;
        next_offset_ = offset + total;
        if (offset == 0 && live_ > 1) {
            wrapped_ = true;
        }
        index_.insert({key_view, slot});
        return Kept::kept;
    }

    /**
     * @brief A report for the command (@p comp_id, @p cl_ord_id) has arrived.
     * @return Whether a copy of it was held.
     */
    bool answered(std::string_view comp_id, std::string_view cl_ord_id) {
        const auto found = find(comp_id, cl_ord_id);
        if (found == index_.end()) {
            return false;
        }
        mark_answered(found->second);
        index_.erase(found);
        return true;
    }

    /// Hands every unanswered copy to @p visit, as visit(envelope, size), oldest first. The bytes are valid only during the call.
    template <typename Visit> void for_each_unanswered(Visit&& visit) const {
        for (size_t position = 0; position < live_; ++position) {
            const Entry& entry = entries_[(head_ + position) % entries_.size()];
            if (!entry.answered) {
                visit(bytes_.data() + entry.offset + entry.key_size, entry.envelope_size);
            }
        }
    }

    /**
     * @brief Forgets every unanswered copy of the member @p comp_id, whose session has ended for good.
     * @return How many were forgotten.
     */
    size_t drop_session(std::string_view comp_id) {
        size_t dropped = 0;
        for (size_t position = 0; position < live_; ++position) {
            const size_t slot = (head_ + position) % entries_.size();
            const Entry& entry = entries_[slot];
            if (entry.answered) {
                continue;
            }
            const std::string_view key = key_of(entry);
            if (key.size() > comp_id.size() && key.compare(0, comp_id.size(), comp_id) == 0 && key[comp_id.size()] == static_cast<char>(separator)) {
                index_.erase(key);
                mark_answered(slot);
                ++dropped;
            }
        }
        return dropped;
    }

    /// How many commands are held unanswered.
    [[nodiscard]] size_t unanswered() const {
        return unanswered_;
    }

    /// Whether the command (@p comp_id, @p cl_ord_id) is held unanswered.
    [[nodiscard]] bool holds(std::string_view comp_id, std::string_view cl_ord_id) const {
        return find(comp_id, cl_ord_id) != index_.end();
    }

  private:
    struct Entry {
        size_t offset{0};
        size_t key_size{0};
        size_t envelope_size{0};
        bool answered{true};
    };

    // Strings hashed by their contents; the keys are views into the block.
    struct KeyHash {
        size_t operator()(std::string_view key) const {
            return std::hash<std::string_view>{}(key);
        }
    };
    using Index = tsl::robin_map<std::string_view, size_t, KeyHash, std::equal_to<std::string_view>>;

    // The separator between the comp id and the ClOrdID in a key. It cannot occur in a FIX field value.
    static constexpr uint8_t separator = 0x01;
    static constexpr size_t no_room = static_cast<size_t>(-1);

    [[nodiscard]] static Index make_index(size_t capacity_bytes, size_t capacity_commands) {
        if (capacity_bytes == 0 || capacity_commands == 0) {
            throw pubsub_itc_fw::PreconditionAssertion("UnansweredCommandStore: both capacities must be greater than zero", __FILE__, __LINE__);
        }
        Index index;
        // Reserved for every command the list can hold, so that the index never grows, and so never
        // allocates, after the store is created.
        index.reserve(capacity_commands);
        return index;
    }

    [[nodiscard]] Index::const_iterator find(std::string_view comp_id, std::string_view cl_ord_id) const {
        // The key is assembled in a small buffer on the stack. A comp id or ClOrdID too long for it is
        // longer than any the gateways accept, and cannot have been kept.
        std::array<char, 256> buffer{};
        if (comp_id.size() + 1 + cl_ord_id.size() > buffer.size()) {
            return index_.end();
        }
        std::memcpy(buffer.data(), comp_id.data(), comp_id.size());
        buffer[comp_id.size()] = static_cast<char>(separator);
        std::memcpy(buffer.data() + comp_id.size() + 1, cl_ord_id.data(), cl_ord_id.size());
        return index_.find(std::string_view(buffer.data(), comp_id.size() + 1 + cl_ord_id.size()));
    }

    [[nodiscard]] std::string_view key_of(const Entry& entry) const {
        return std::string_view(reinterpret_cast<const char*>(bytes_.data() + entry.offset), entry.key_size);
    }

    void mark_answered(size_t slot) {
        if (!entries_[slot].answered) {
            entries_[slot].answered = true;
            --unanswered_;
        }
    }

    // Reuses the space of answered copies at the oldest end, stopping at the first copy still unanswered.
    void reclaim_answered() {
        while (live_ > 0 && entries_[head_].answered) {
            const size_t offset = entries_[head_].offset;
            head_ = (head_ + 1) % entries_.size();
            --live_;
            if (live_ == 0) {
                next_offset_ = 0;
                wrapped_ = false;
            } else if (wrapped_ && entries_[head_].offset < offset) {
                wrapped_ = false;
            }
        }
    }

    // The offset at which @p size bytes fit without overwriting a copy still held, or no_room.
    //
    // While nothing has been written at the start of the block, the copies lie in one stretch, from
    // the oldest copy's offset to next_offset_. Once one has, they lie in two: from the oldest copy's
    // offset to where the copies before the wrap ended, and from the start of the block to
    // next_offset_. wrapped_ says which.
    [[nodiscard]] size_t find_room(size_t size) const {
        if (live_ == 0) {
            return 0;
        }
        const size_t oldest_offset = entries_[head_].offset;
        if (!wrapped_) {
            if (bytes_.size() - next_offset_ >= size) {
                return next_offset_;
            }
            return oldest_offset >= size ? 0 : no_room;
        }
        return oldest_offset - next_offset_ >= size ? next_offset_ : no_room;
    }

    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    Index index_;
    size_t head_{0};
    size_t live_{0};
    size_t unanswered_{0};
    size_t next_offset_{0};
    bool wrapped_{false};
};

} // namespaces
