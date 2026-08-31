#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Establishes that a catch-up received every record it should have, and says so plainly.
 *
 * A component recovering from a checkpoint presents a position and is sent everything after it.
 * Those records are applied *silently* -- without repeating the execution reports that were
 * produced the first time -- so nothing about applying them draws attention, and nothing about
 * failing to receive one does either. A catch-up missing a record therefore leaves a component
 * whose state is wrong and which believes it is current, and every check it makes afterwards is
 * made against the wrong state. That is R-0101, and this is what answers it.
 *
 * **Why a counter is enough.** The sequencer assigns numbers with `next_sequence_number_++` and
 * never skips one. So "did I receive every record between the position I presented and the
 * position I reached" is not a protocol question -- it is arithmetic. The records must be
 * exactly P+1, P+2 ... Q, in that order, with nothing missing and nothing repeated.
 *
 * Strictness is deliberate. Anything other than the expected number means the stream is not what
 * it claims to be, and a component that cannot say what it holds must not begin acting on it.
 *
 * Typical use:
 * @code
 *   CatchUpTally tally(position_presented);
 *   // for each record streamed to us:
 *   tally.offer(record.seq_no);                     // the answer may be left until the end
 *   // when the authority says where its record ends:
 *   if (!tally.complete_through(ack.last_seq_no))
 *       halt(tally.describe_shortfall(ack.last_seq_no));   // R-0101: do not begin acting
 * @endcode
 */
class CatchUpTally {
  public:
    /**
     * @param[in] position_presented The position handed to the authority: the last record this
     *                               component already holds. The first record expected is the
     *                               one after it. Zero means "I hold nothing", so the first
     *                               record expected is 1.
     */
    explicit CatchUpTally(int64_t position_presented) : presented_(position_presented), expected_next_(position_presented + 1) {}

    /**
     * @brief Offers the next record's sequence number.
     *
     * @return true if it is the one expected. false if it is not, in which case the first such
     *         discrepancy is retained and reported by describe_shortfall(). Offering more
     *         records after a discrepancy is harmless and does not overwrite the first one,
     *         which is the one worth reporting.
     */
    bool offer(int64_t seq_no) {
        if (seq_no != expected_next_) {
            if (!broken_) {
                broken_ = true;
                broke_at_expected_ = expected_next_;
                broke_at_received_ = seq_no;
            }
            return false;
        }
        ++expected_next_;
        ++received_;
        return true;
    }

    /**
     * @brief Whether everything from the presented position through @p head arrived.
     *
     * @param[in] head The last record the authority holds: the far end of the range. A head
     *                 equal to the presented position means there was nothing to catch up on,
     *                 which is complete rather than suspicious.
     */
    [[nodiscard]] bool complete_through(int64_t head) const {
        if (broken_) {
            return false;
        }
        // A head behind the presented position means this component holds records the authority
        // does not. That is not a shortfall, it is a contradiction, and it must not read as
        // success just because nothing was missing from a range that runs backwards.
        if (head < presented_) {
            return false;
        }
        return expected_next_ == head + 1;
    }

    /**
     * @brief Why the catch-up was not complete, in terms a person reading a log can act on.
     *
     * Returns an empty string when it was complete. The wording names the numbers rather than
     * summarising them, because the first question anyone asks is which records are missing.
     */
    [[nodiscard]] std::string describe_shortfall(int64_t head) const {
        if (complete_through(head)) {
            return "";
        }
        if (broken_) {
            return "the record stream jumped: expected seq_no " + std::to_string(broke_at_expected_) + " and received " + std::to_string(broke_at_received_) +
                   ", so " + std::to_string(broke_at_received_ - broke_at_expected_) + " record(s) were not delivered";
        }
        if (head < presented_) {
            return "the authority's record ends at seq_no " + std::to_string(head) + ", behind the position presented (" + std::to_string(presented_) +
                   "), so this component holds records the authority does not";
        }
        return "the stream stopped early: " + std::to_string(head + 1 - expected_next_) + " record(s) between seq_no " + std::to_string(expected_next_) +
               " and " + std::to_string(head) + " were never delivered";
    }

    /// The position handed to the authority.
    [[nodiscard]] int64_t presented() const {
        return presented_;
    }

    /// The sequence number the next record must carry.
    [[nodiscard]] int64_t expected_next() const {
        return expected_next_;
    }

    /// How many records were accepted in order. Worth logging even on success: a catch-up that
    /// applied nothing and one that never ran look identical otherwise.
    [[nodiscard]] int64_t received() const {
        return received_;
    }

  private:
    int64_t presented_{0};
    int64_t expected_next_{1};
    int64_t received_{0};

    bool broken_{false};
    int64_t broke_at_expected_{0};
    int64_t broke_at_received_{0};
};

} // namespaces
