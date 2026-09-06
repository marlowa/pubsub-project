#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Establishes that a catch-up received every record that was sent to it, and says so plainly.
 *
 * A component recovering from a checkpoint presents a position and is sent what follows it. Those
 * records are applied *silently* -- without repeating the execution reports produced the first
 * time -- so nothing about applying them draws attention, and nothing about failing to receive one
 * does either. A catch-up missing a record leaves a component whose state is wrong and which
 * believes it is current, and every check it makes afterwards is made against the wrong state.
 * That is R-0101, and this is what answers it.
 *
 * **Why the check is a count and not a run of consecutive numbers.** The obvious reading is that
 * the records must be exactly P+1, P+2 ... Q, because the authority numbers them with an
 * incrementing counter and never skips one. That reading is wrong by construction and was built
 * once before it was: the stream is deliberately filtered. A sequencer streaming a matching engine
 * back into currency forwards the orders and the cancels, and withholds the execution report
 * envelopes, which are outputs rather than inputs. So the receiver is sent a *subset* of the
 * numbers in the range, and a gap in what arrives is ordinary rather than a fault.
 *
 * What can be checked is therefore narrower, and is stated here so that nobody has to infer it:
 * every record the authority *sent* arrived, exactly once, in ascending order. It does not
 * establish that the authority sent the right records. A filter that wrongly withheld one would be
 * invisible, because a component cannot verify it received what it was never told about.
 *
 * Typical use:
 * @code
 *   CatchUpTally tally(position_presented);
 *   // for each record streamed to us:
 *   tally.offer(record.seq_no);
 *   // when the authority says how many it sent and where its record ends:
 *   if (!tally.complete_with(ack.records_sent, ack.last_seq_no))
 *       halt(tally.describe_shortfall(ack.records_sent, ack.last_seq_no));   // R-0101: do not act
 * @endcode
 */
class CatchUpTally {
  public:
    /**
     * @param[in] position_presented The position handed to the authority: the last record this
     *                               component already holds. Every record sent must be numbered
     *                               after it. A negative value means "I hold nothing and have
     *                               applied nothing", which the authority answers by placing the
     *                               component at its head rather than by streaming history.
     */
    explicit CatchUpTally(int64_t position_presented) : presented_(position_presented), last_accepted_(position_presented) {}

    /**
     * @brief Offers the next record's sequence number.
     *
     * @return true if it may be counted: numbered after the position presented, and after every
     *         record already accepted. false otherwise, in which case the first such discrepancy
     *         is retained and reported by describe_shortfall(). Offering more records after a
     *         discrepancy is harmless and does not overwrite the first one, which is the one worth
     *         reporting.
     */
    bool offer(int64_t seq_no) {
        if (seq_no <= last_accepted_) {
            if (!broken_) {
                broken_ = true;
                broke_after_ = last_accepted_;
                broke_at_received_ = seq_no;
            }
            return false;
        }
        last_accepted_ = seq_no;
        ++received_;
        return true;
    }

    /**
     * @brief Whether exactly what the authority sent arrived, in order and none repeated.
     *
     * @param[in] records_sent How many records the authority says it streamed. Zero is complete
     *                         rather than suspicious: there may have been nothing to send.
     * @param[in] head         The last record the authority holds: the far end of the range.
     */
    [[nodiscard]] bool complete_with(int64_t records_sent, int64_t head) const {
        if (broken_) {
            return false;
        }
        // A head behind the presented position means this component holds records the authority
        // does not. That is not a shortfall, it is a contradiction, and it must not read as
        // success just because the count happened to agree.
        if (head < presented_) {
            return false;
        }
        return received_ == records_sent;
    }

    /**
     * @brief Why the catch-up was not complete, in terms a person reading a log can act on.
     *
     * Returns an empty string when it was complete. The wording names the numbers rather than
     * summarising them, because the first question anyone asks is how much is missing.
     */
    [[nodiscard]] std::string describe_shortfall(int64_t records_sent, int64_t head) const {
        if (complete_with(records_sent, head)) {
            return "";
        }
        if (broken_) {
            return "the record stream did not advance: seq_no " + std::to_string(broke_at_received_) + " arrived after seq_no " + std::to_string(broke_after_) +
                   ", so a record was repeated or the stream went backwards";
        }
        if (head < presented_) {
            return "the authority's record ends at seq_no " + std::to_string(head) + ", behind the position presented (" + std::to_string(presented_) +
                   "), so this component holds records the authority does not";
        }
        if (received_ < records_sent) {
            return "the stream stopped early: " + std::to_string(records_sent) + " record(s) were sent and " + std::to_string(received_) + " arrived, so " +
                   std::to_string(records_sent - received_) + " never did";
        }
        return "more records arrived than were sent: " + std::to_string(records_sent) + " were sent and " + std::to_string(received_) + " arrived";
    }

    /// The position handed to the authority.
    [[nodiscard]] int64_t presented() const {
        return presented_;
    }

    /// The highest sequence number accepted so far, or the presented position if none has been.
    [[nodiscard]] int64_t last_accepted() const {
        return last_accepted_;
    }

    /// How many records were accepted. Worth logging even on success: a catch-up that applied
    /// nothing and one that never ran look identical otherwise.
    [[nodiscard]] int64_t received() const {
        return received_;
    }

  private:
    int64_t presented_{0};
    int64_t last_accepted_{0};
    int64_t received_{0};

    bool broken_{false};
    int64_t broke_after_{0};
    int64_t broke_at_received_{0};
};

} // namespaces
