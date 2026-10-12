#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/LeasePromiseRecord.hpp>

namespace pubsub_itc_fw {

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
 *
 * record() writes and syncs before it returns, so it can take a long time on a slow disk. A thread that
 * handles orders never calls it: BackgroundPromiseRecorder calls it on a thread of its own.
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
};

} // namespaces
