#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>

namespace sequencer {

/**
 * @brief Measures how long the venue has had no matching engine that can trade, for the limit on automatic recovery
 *        (RecoveryLimitRule).
 *
 * The measure is about the venue, not one engine. An engine that is down while its peer trades is a loss of resilience
 * and nothing more, so no engine measures its own absence; the leading sequencer does, because it routes orders only to
 * the engine that leads and so knows when it has none to route to (EngineOrderRouting).
 *
 * The absence is measured in one of two ways.
 *
 * - **While this process runs**, from the first moment it saw no engine to route to. observe() is called about once a
 *   second, and whenever the routing changes.
 * - **When the whole venue was down**, sequencers included, from the last moment this process knew of an engine it
 *   routed to, which it keeps in a small file beside its WAL (EngineRoutedStamp), but no earlier than the first record
 *   in the WAL: before the first record there was nothing to recover. The last record in the WAL would be the wrong
 *   measure, because a quiet spell before the venue went down would be counted as absence, and orders that arrive while
 *   no engine runs are still written and would shorten it. When another instance is seen to lead, what this process
 *   measured at start-up does not describe the venue, and the absence is measured from then on as while running.
 *
 * An absence ends when an engine is routed to again.
 *
 * It measures; it does not decide, log, write or send. One thread, the sequencer's, calls every member function. Times
 * are wall-clock nanoseconds since the Unix epoch, as the WAL records them.
 */
class EngineAbsenceRule {
  public:
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

    /// When the present absence began, or 0 if an engine is routed to or nothing is known yet.
    [[nodiscard]] int64_t absence_began_ns() const {
        return absence_began_ns_;
    }

  private:
    int64_t absence_began_ns_{0};
};

} // namespaces
