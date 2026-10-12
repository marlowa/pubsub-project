#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>

#include <LeasePromiseRecord.hpp>

namespace fix_common {

/**
 * @brief Somewhere an instance records the promise it has made as a voter, so that a restart does not forget it.
 *
 * A voter that restarts having forgotten what it promised must grant nothing for one lease period,
 * because it cannot tell whether it has already promised its vote to someone else. An instance that
 * has recorded its promise has not forgotten, and can vote, and ask to lead, as soon as it restarts.
 * That is what lets a process restarted by its supervisor within the lease period keep the lead, rather
 * than being overtaken by its peer. See docs/availability/majority_leases.md, rule 6.
 *
 * The record also holds the newest leader's statement the instance holds about which instance may not
 * lead (LeaderStatement), so that a restart does not forget that either.
 */
class LeasePromiseRecorderInterface {
  public:
    virtual ~LeasePromiseRecorderInterface() = default;

    /**
     * @brief Record, durably, the promise this instance has made and the statement it holds.
     * @param[in] record The promise, whose expiry may safely be later than the true one, and the statement.
     * @return true when the record reached the disk. When it did not, the caller must not grant the lease.
     */
    [[nodiscard]] virtual bool record(const LeasePromiseRecord& record) = 0;

    /**
     * @brief Start writing a record without waiting for it, if this recorder can.
     *
     * Lets the thread that handles leases refresh a record well before it is needed, so that it does
     * not stop answering lease requests while the disk is written (BUG-0107). The record must not be
     * relied on until background_result() has reported it written. A recorder that cannot write in
     * the background returns false and the caller writes with record() when it must.
     *
     * @return true when the write was started; false when it was not, and nothing was asked for.
     */
    [[nodiscard]] virtual bool record_in_background(const LeasePromiseRecord& /*record*/) {
        return false;
    }

    /**
     * @brief The outcome of the write record_in_background() started, once it has finished.
     *
     * @return std::nullopt while no write has finished since the last call; otherwise true when the
     *         record reached the disk and false when it did not. Each outcome is reported once.
     */
    [[nodiscard]] virtual std::optional<bool> background_result() {
        return std::nullopt;
    }
};

} // namespaces
