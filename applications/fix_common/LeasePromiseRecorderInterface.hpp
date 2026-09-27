#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

namespace fix_common {

/**
 * @brief Somewhere an instance records the promise it has made as a voter, so that a restart does not forget it.
 *
 * A voter that restarts having forgotten what it promised must grant nothing for one lease period,
 * because it cannot tell whether it has already promised its vote to someone else. An instance that
 * has recorded its promise has not forgotten, and can vote, and ask to lead, as soon as it restarts.
 * That is what lets a process restarted by its supervisor within the lease period keep the lead, rather
 * than being overtaken by its peer. See docs/availability/majority_leases.md, rule 6.
 */
class LeasePromiseRecorderInterface {
  public:
    virtual ~LeasePromiseRecorderInterface() = default;

    /**
     * @brief Record, durably, that this instance has promised its vote to @p promised_to until @p until.
     * @param[in] promised_to The instance promised to, or zero for no promise.
     * @param[in] until When the promise runs out, on the steady clock. A later time than the true one is safe.
     * @return true when the record reached the disk. When it did not, the caller must not grant the lease.
     */
    [[nodiscard]] virtual bool record(int64_t promised_to, std::chrono::steady_clock::time_point until) = 0;
};

} // namespaces
