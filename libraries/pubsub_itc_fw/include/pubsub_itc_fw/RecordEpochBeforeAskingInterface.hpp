#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Writes the epoch an instance is about to ask to lead in to disk, before the request is sent.
 *
 * PairLeaseAgent calls this between choosing the epoch it will ask to lead in and sending the request, which is rule
 * 13 of docs/availability/majority_leases.md. The lease is counted from the moment the request is sent, so the time
 * the disk takes does not come out of the lease, and when the grant arrives the epoch is already on disk and the
 * instance acts on it at once. Without it the epoch is written once the grant has arrived, while the lease runs, and a
 * busy disk can take longer than the lease (docs/bug_list.md, BUG-0228).
 *
 * The write waits for the disk on the calling thread. That is the exception principle F4 of
 * docs/design_principles.md allows while an instance changes role: an instance asking to lead is handling no orders.
 *
 * EpochRecordedBeforeAsking is the implementation the sequencer and the matching engine use. An owner that keeps no
 * epoch on disk, as the arbiter and the matching engine publisher do not, gives the agent none.
 */
class RecordEpochBeforeAskingInterface {
  public:
    virtual ~RecordEpochBeforeAskingInterface() = default;

    /**
     * @brief Write @p epoch to disk, and return once the write has finished or failed.
     *
     * A failure is the implementation's to report. The agent asks to lead whatever the outcome, because stopping
     * would take out an instance that is otherwise healthy; what a failure loses is the guarantee that a restart does
     * not reuse the epoch.
     *
     * @return How long the write took, measured on the steady clock. The agent adds it to the time it was given, so
     *         that the time it notes for sending the request is after the write, whichever clock the caller keeps
     *         the lease by.
     */
    [[nodiscard]] virtual std::chrono::steady_clock::duration record_before_asking(int32_t epoch) = 0;
};

} // namespaces
