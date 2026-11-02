#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief Whether trading is halted, as the TechnicalEvent records in the sequencer's WAL say, and the last member's
 *        new order or cancel written before each of them.
 *
 * The sequencer keeps this from the WAL in the same three ways as it keeps the voids: as the replicated WAL reads the
 * whole WAL when it opens, as this instance writes records as leader, and as it writes its leader's records as follower
 * (docs/availability/voided_commands.md, section 6.1). The latest TechnicalEvent says whether trading is halted.
 *
 * The last member's command before a halt is what decides whether an operator may lift it: the halt is lifted only once
 * the WAL holds a report answering that command or a later one (section 6.2). The matching engine applies commands one
 * at a time, in the order of their positions, and sends every report of a command before it applies the next, so a
 * report answering that command means every member's command before the halt has been answered.
 *
 * Records after a position are discarded when a follower's WAL is cut back to match its leader's. The positions of the
 * most recent members' commands are kept for that, so that the last one left is known exactly. When a discard removes
 * more of them than are kept, the position the WAL is cut back to is taken instead: no member's command left in the
 * WAL is later than it, so a lift is never allowed early, though one may be declined until a report answers a later
 * command.
 */
class TradingHaltState {
  public:
    /// One TechnicalEvent record in the WAL.
    struct TechnicalEventRecord {
        int64_t seq_no{0};                                                                                             ///< its position
        bool halted{false};                                                                                            ///< whether it halts trading
        pubsub_itc_fw_app::TechnicalEventReason reason{pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook}; ///< why it was written
        int64_t last_members_command_before{0}; ///< the last member's new order or cancel before it; zero when there is none
    };

    /// Whether an operator's request to lift the halt may be acted on, and if not, why not.
    enum class Lift : int8_t {
        Allowed,           ///< trading is halted, a matching engine leads, and every member's command before the halt is answered
        NotHalted,         ///< trading is not halted, so there is nothing to lift
        NoEngineLeads,     ///< no matching engine leads, so lifted now the venue would take orders with nothing to trade them
        ReportsOutstanding ///< the WAL holds no report yet answering the last member's command before the halt
    };

    /// How many of the most recent members' commands are kept, so that a discard can find the last one left.
    static constexpr size_t recent_members_commands_kept = 1024;

    /// A member's new order or cancel at @p seq_no, which is later than every record noted before it.
    void note_members_command(int64_t seq_no) {
        if (recent_count_ == recent_members_commands_kept) {
            // The oldest is overwritten; it is the latest member's command older than every one still kept.
            latest_forgotten_ = recent_[recent_oldest_];
            recent_oldest_ = (recent_oldest_ + 1) % recent_members_commands_kept;
            --recent_count_;
        }
        recent_[(recent_oldest_ + recent_count_) % recent_members_commands_kept] = seq_no;
        ++recent_count_;
    }

    /**
     * @brief A TechnicalEvent at @p seq_no, which is later than every record noted before it.
     * @return true when it changes whether trading is halted.
     */
    bool note_technical_event(int64_t seq_no, bool halted, pubsub_itc_fw_app::TechnicalEventReason reason) {
        const bool was_halted = this->halted();
        events_.push_back(TechnicalEventRecord{seq_no, halted, reason, last_members_command()});
        return was_halted != halted;
    }

    /**
     * @brief Forgets every record after @p seq_no, which the WAL no longer holds.
     * @return true when it changes whether trading is halted.
     */
    bool discard_after(int64_t seq_no) {
        const bool was_halted = halted();
        while (!events_.empty() && events_.back().seq_no > seq_no) {
            events_.pop_back();
        }
        while (recent_count_ > 0 && recent_[(recent_oldest_ + recent_count_ - 1) % recent_members_commands_kept] > seq_no) {
            --recent_count_;
        }
        if (latest_forgotten_ > seq_no) {
            // More members' commands were discarded than are kept: the last one left is at or before seq_no.
            latest_forgotten_ = seq_no;
        }
        return was_halted != halted();
    }

    /// Whether trading is halted: whether the latest TechnicalEvent in the WAL halts it.
    [[nodiscard]] bool halted() const {
        return !events_.empty() && events_.back().halted;
    }

    /// The latest TechnicalEvent in the WAL, if it holds one.
    [[nodiscard]] std::optional<TechnicalEventRecord> latest() const {
        if (events_.empty()) {
            return std::nullopt;
        }
        return events_.back();
    }

    /**
     * @brief Whether the halt may be lifted (docs/availability/voided_commands.md, section 6.2).
     * @param[in] engine_leads             Whether a matching engine leads and is sent orders.
     * @param[in] highest_command_answered The highest command a report in the WAL answers.
     * @return Allowed only if trading is halted, an engine leads, and a report answers the last member's command before
     *         the halt, or a later one.
     */
    [[nodiscard]] Lift may_lift(bool engine_leads, int64_t highest_command_answered) const {
        if (!halted()) {
            return Lift::NotHalted;
        }
        if (!engine_leads) {
            return Lift::NoEngineLeads;
        }
        if (events_.back().last_members_command_before > highest_command_answered) {
            return Lift::ReportsOutstanding;
        }
        return Lift::Allowed;
    }

    /// The last member's new order or cancel noted, or zero when none is.
    [[nodiscard]] int64_t last_members_command() const {
        if (recent_count_ > 0) {
            return recent_[(recent_oldest_ + recent_count_ - 1) % recent_members_commands_kept];
        }
        return latest_forgotten_;
    }

  private:
    // Every TechnicalEvent in the WAL, oldest first. A day has few.
    std::vector<TechnicalEventRecord> events_;
    // The most recent members' commands, oldest at recent_oldest_, in a ring, so that noting one allocates nothing.
    std::array<int64_t, recent_members_commands_kept> recent_{};
    size_t recent_oldest_{0};
    size_t recent_count_{0};
    // The latest member's command no longer in recent_, or zero.
    int64_t latest_forgotten_{0};
};

} // namespaces
