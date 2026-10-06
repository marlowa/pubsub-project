#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <mutex>

namespace sequencer {

/**
 * @brief Writes the records a leader sends into a follower's write-ahead log, one at a time, whichever
 *        thread delivers them.
 *
 * A follower receives its leader's records on two threads. The reactor's thread writes them as they
 * arrive, through a handler installed on the connection to the peer, while the two logs are known to
 * agree; records it does not write are passed to the sequencer's thread, which writes them there. The
 * log's writer is not built to be used by two threads at once. A follower's log was found with one
 * entry's bytes blank and the next record written twice (docs/bug_list.md, BUG-0123), which is what two
 * threads appending at once would leave.
 *
 * So every write of a replicated record goes through write_if_next(), which decides whether the record
 * is the next one the log needs and writes it in one step, under a lock; and anything else that changes
 * the log while records may be arriving, such as discarding records the leader does not hold, does so
 * through change_log(), under the same lock. Then, whichever thread delivers a record and in whatever
 * order, a record is written only if it follows the last one written, so the log holds every record
 * once, in order, with nothing between them.
 *
 * The lock is taken on every replicated record, from the follower's threads only; a leader writes its
 * own records on one thread and does not use this. Uncontended, as it nearly always is, it costs a
 * few tens of nanoseconds, against a write to the log that costs microseconds.
 */
class ReplicatedRecordWriter {
  public:
    enum class Outcome {
        /// The record was the next one, and was written.
        written,
        /// The log already holds a record with this number; nothing was written.
        already_held,
        /// A record before this one is missing; nothing was written.
        gap
    };

    /**
     * @brief Writes record @p seq_no if it is the next one the log needs.
     *
     * @param[in] seq_no       The record's number.
     * @param[in] last_written Returns the number of the last record in the log.
     * @param[in] write        Writes the record.
     * @return What was done.
     */
    template <typename LastWritten, typename Write> Outcome write_if_next(int64_t seq_no, LastWritten&& last_written, Write&& write) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const int64_t last = last_written();
        if (seq_no <= last) {
            return Outcome::already_held;
        }
        if (seq_no > last + 1) {
            return Outcome::gap;
        }
        write();
        return Outcome::written;
    }

    /// Runs @p change, which changes the log, while no replicated record is being written.
    template <typename Change> void change_log(Change&& change) {
        const std::lock_guard<std::mutex> lock(mutex_);
        change();
    }

  private:
    std::mutex mutex_;
};

} // namespaces
