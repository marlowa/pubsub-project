#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>

#include <pubsub_itc_fw/LeasePromiseRecord.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Writes an instance's lease promise records without the calling thread waiting for the disk.
 *
 * PairLeaseAgent keeps its promises through this interface. The agent runs on the thread that handles
 * orders in the sequencer and the matching engine, and that thread never waits for the disk (principle
 * F4 in docs/design_principles.md). So the agent only ever starts a write and later asks whether it has
 * finished; it never writes a record itself. A promise that the record on disk does not yet cover is
 * refused until the write that covers it has finished.
 *
 * BackgroundPromiseRecorder is the implementation every program uses. It writes on a thread of its own,
 * through a LeasePromiseRecorderInterface such as LeasePromiseStore.
 */
class BackgroundPromiseRecorderInterface {
  public:
    virtual ~BackgroundPromiseRecorderInterface() = default;

    /**
     * @brief Start writing @p record, and return without waiting for it.
     *
     * Only one write is in progress at a time. The record must not be relied on until background_result()
     * has reported it written.
     *
     * @return true when the write was started; false when one is already in progress, and nothing was started.
     */
    [[nodiscard]] virtual bool record_in_background(const LeasePromiseRecord& record) = 0;

    /**
     * @brief The outcome of the write record_in_background() started, once it has finished.
     *
     * @return std::nullopt while no write has finished since the last call; otherwise true when the record
     *         reached the disk and false when it did not. Each outcome is reported once.
     */
    [[nodiscard]] virtual std::optional<bool> background_result() = 0;
};

} // namespaces
