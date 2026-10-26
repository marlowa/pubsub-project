#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace sequencer {

/**
 * @brief Decides when the leading sequencer writes the command that R-0117 asks for: no matching engine could trade for
 * longer than the venue allows, so the orders open at the time are cancelled, each member told, and trading halted.
 *
 * The rule is about the venue, not one engine. An engine that is down while its peer trades is a loss of resilience and
 * nothing more, so no engine measures its own absence; the leading sequencer does, because it routes orders only to the
 * engine that leads and so knows when it has none to route to (EngineOrderRouting). When the absence is longer than the
 * limit, it writes a CancelBook command that cancels every order on the book and halts trading if it cancels any. The
 * decision is a command in the WAL, so every engine applies it at the same position, one that catches up later
 * included (docs/availability/deterministic_matching_engine.md, section 4.2).
 *
 * The absence is measured in one of two ways.
 *
 * - **While this process runs**, from the first moment it saw no engine to route to. observe() is called about once a
 *   second, and whenever the routing changes.
 * - **When the whole venue was down**, sequencers included, from the last moment this process knew of an engine it
 *   routed to, which it keeps in a small file beside its WAL (EngineRoutedStamp), but no earlier than the first record
 *   in the WAL: before the first record there was nothing to cancel. The last record in the WAL would be the wrong
 *   measure, because a quiet spell before the venue went down would be counted as absence, and orders that arrive while
 *   no engine runs are still written and would shorten it. When another instance is seen to lead, what this process
 *   measured at start-up does not describe the venue, and the absence is measured from then on as while running.
 *
 * The command is written once for each absence, and only while this instance leads and its WAL holds any record: an
 * empty WAL holds no order to cancel. An absence ends when an engine is routed to again.
 *
 * It decides; it does not log, write or send. The owner does those. One thread, the sequencer's, calls every member
 * function. Times are wall-clock nanoseconds since the Unix epoch, as the WAL records them.
 */
class EngineAbsenceRule {
  public:
    explicit EngineAbsenceRule(std::chrono::seconds limit) : limit_ns_{std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count()} {}

    /**
     * @brief What this process found at start-up.
     * @param[in] engine_routed_at_ns The last time this process knew of an engine it routed orders to, from the stamp
     *                                beside its WAL, or 0 if there is no stamp.
     * @param[in] first_record_ns     The wall time of the first record in the WAL, or 0 if the WAL is empty.
     */
    void started(int64_t engine_routed_at_ns, int64_t first_record_ns) {
        absence_began_ns_ = first_record_ns == 0 ? 0 : std::max(engine_routed_at_ns, first_record_ns);
    }

    /**
     * @brief Whether an engine is routed to now. Called about once a second, and whenever the routing changes.
     */
    void observe(bool engine_routed, int64_t now_ns) {
        if (engine_routed) {
            absence_began_ns_ = 0;
            command_written_ = false;
            return;
        }
        if (absence_began_ns_ == 0) {
            absence_began_ns_ = now_ns;
        }
    }

    /// Another instance leads, so the venue was not down as a whole; an absence is measured from now, if there is one.
    void another_instance_leads(int64_t now_ns) {
        if (absence_began_ns_ != 0) {
            absence_began_ns_ = now_ns;
        }
    }

    /**
     * @brief Whether the command is due now.
     * @param[in] now_ns            The time now.
     * @param[in] leads             Whether this instance leads, which is the only instance that writes commands.
     * @param[in] wal_holds_records Whether the WAL holds any record.
     */
    [[nodiscard]] bool command_due(int64_t now_ns, bool leads, bool wal_holds_records) const {
        return leads && wal_holds_records && absence_began_ns_ != 0 && !command_written_ && now_ns - absence_began_ns_ > limit_ns_;
    }

    /// The command has been written for this absence; it is not written again until an engine has been routed to.
    void command_written() {
        command_written_ = true;
    }

    /// When the present absence began, or 0 if an engine is routed to or nothing is known yet.
    [[nodiscard]] int64_t absence_began_ns() const {
        return absence_began_ns_;
    }

    /// The limit, in nanoseconds.
    [[nodiscard]] int64_t limit_ns() const {
        return limit_ns_;
    }

  private:
    int64_t limit_ns_;
    int64_t absence_began_ns_{0};
    bool command_written_{false};
};

} // namespaces
