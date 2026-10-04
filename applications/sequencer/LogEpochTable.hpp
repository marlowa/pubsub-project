#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace sequencer {

/**
 * @brief Which leadership wrote each record of a sequencer's write-ahead log, held as the record at
 *        which each run of records from one epoch begins.
 *
 * A rejoining follower and its leader find the last record their logs agree on by comparing the
 * epochs of their records (docs/availability/follower_log_repair.md). Each record carries the epoch
 * of the leadership that sequenced it; this table answers questions about them without reading the
 * log. The sequencer builds it while it reads its log at startup and keeps it up to date as it writes.
 * The epoch changes only once per change of leader, so the table holds a handful of entries.
 *
 * Records are numbered from 1 without a break, which the sequencer makes true when it opens its log.
 * Epochs usually rise from one run to the next, but are not assumed to: a new leader can briefly lead
 * below an epoch already led in (docs/availability/tla/findings.md, section 11.5).
 *
 * One thread, the sequencer's, calls every member function.
 */
class LogEpochTable {
  public:
    /**
     * @brief A record has been appended.
     * @param[in] seq_no Its sequence number, one more than the last.
     * @param[in] epoch The epoch it carries; zero for a record written before records carried one.
     */
    void note_record(int64_t seq_no, int32_t epoch) {
        if (seq_no != last_seq_no_ + 1) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LogEpochTable: record {} follows record {}", seq_no, last_seq_no_), __FILE__, __LINE__);
        }
        if (runs_.empty() || runs_.back().epoch != epoch) {
            runs_.push_back(Run{epoch, seq_no});
        }
        last_seq_no_ = seq_no;
    }

    /// The sequence number of the last record, or zero for an empty log.
    [[nodiscard]] int64_t last_seq_no() const {
        return last_seq_no_;
    }

    /// The epoch of the last record, or zero for an empty log.
    [[nodiscard]] int32_t last_epoch() const {
        return runs_.empty() ? 0 : runs_.back().epoch;
    }

    /// The epoch of record @p seq_no, which must be in the log.
    [[nodiscard]] int32_t epoch_of(int64_t seq_no) const {
        if (seq_no < 1 || seq_no > last_seq_no_) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LogEpochTable::epoch_of: record {} is not in a log of {}", seq_no, last_seq_no_), __FILE__,
                                                       __LINE__);
        }
        for (auto run = runs_.rbegin(); run != runs_.rend(); ++run) {
            if (run->first_seq_no <= seq_no) {
                return run->epoch;
            }
        }
        return 0;
    }

    /// The last record written in epoch @p epoch or an earlier one, or zero if there is none.
    [[nodiscard]] int64_t last_record_at_or_before(int32_t epoch) const {
        int64_t end = last_seq_no_;
        for (auto run = runs_.rbegin(); run != runs_.rend(); ++run) {
            if (run->epoch <= epoch) {
                return end;
            }
            end = run->first_seq_no - 1;
        }
        return 0;
    }

    /// The leader's answer to a follower: a record of the leader's log, and its epoch there.
    struct PositionAnswer {
        int64_t seq_no{0};
        int32_t epoch{0};
    };

    /// What a follower does with the leader's answer: keep its log through keep_through, and whether the logs then agree.
    struct FollowerStep {
        int64_t keep_through{0};
        bool agreed{false};
    };

    /**
     * @brief The leader's answer to a follower whose last record is @p follower_last_seq_no, from @p follower_last_epoch.
     *
     * The last record, at or below the follower's last, that this log holds with an epoch no later than
     * the follower's last epoch, and that record's epoch in this log. Records after it in the follower's
     * log cannot be the same as this log's: either this log has no record there, or its record there
     * is from a later epoch than any the follower holds.
     */
    [[nodiscard]] PositionAnswer answer_position(int64_t follower_last_seq_no, int32_t follower_last_epoch) const {
        const int64_t seq_no = std::min(follower_last_seq_no, last_record_at_or_before(follower_last_epoch));
        return PositionAnswer{seq_no, seq_no == 0 ? 0 : epoch_of(seq_no)};
    }

    /**
     * @brief What a follower, holding this log, does with the leader's answer.
     *
     * It keeps its log through the record answered. If its own record there carries the epoch the
     * leader gave, the two logs agree up to it. If not, it discards the whole run of its records from
     * the epoch of its record there, and asks again. Some of those records may be in the leader's log
     * too; discarding them is safe, because the leader sends them again, and doing so needs nothing to
     * be true of the order of epochs in either log. Records from different epochs differ when an
     * instance whose log lacked records led, which rule 11 prevents once it is in force, or after an
     * epoch went backwards. Each step keeps fewer records, so the exchange ends, and it takes a round
     * for each run discarded rather than one for each record.
     */
    [[nodiscard]] FollowerStep follower_step(const PositionAnswer& answer) const {
        const int64_t seq_no = std::min(answer.seq_no, last_seq_no_);
        if (seq_no <= 0) {
            return FollowerStep{0, true};
        }
        if (epoch_of(seq_no) == answer.epoch) {
            return FollowerStep{seq_no, true};
        }
        return FollowerStep{first_of_run_holding(seq_no) - 1, false};
    }

    /// Records after @p seq_no have been discarded from the log.
    void truncate_after(int64_t seq_no) {
        if (seq_no < 0 || seq_no > last_seq_no_) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LogEpochTable::truncate_after: record {} is not in a log of {}", seq_no, last_seq_no_),
                                                       __FILE__, __LINE__);
        }
        while (!runs_.empty() && runs_.back().first_seq_no > seq_no) {
            runs_.pop_back();
        }
        last_seq_no_ = seq_no;
    }

  private:
    // The first record of the run of records from one epoch that holds record seq_no, which is in the log.
    [[nodiscard]] int64_t first_of_run_holding(int64_t seq_no) const {
        for (auto run = runs_.rbegin(); run != runs_.rend(); ++run) {
            if (run->first_seq_no <= seq_no) {
                return run->first_seq_no;
            }
        }
        return 1;
    }

    struct Run {
        int32_t epoch{0};
        int64_t first_seq_no{0};
    };

    std::vector<Run> runs_;
    int64_t last_seq_no_{0};
};

} // namespaces
