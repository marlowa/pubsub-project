#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <optional>

namespace sequencer {

/**
 * @brief Decides when the leading sequencer halts trading because no matching engine could be trading within the
 *        longest recovery the venue does by itself.
 *
 * While no matching engine leads, the venue cannot answer members. A normal failover takes a lease period and a
 * catch-up of milliseconds; a matching engine catching up from far back can take much longer. The venue states the
 * longest recovery it does by itself, and when the time no engine has led, added to how long the catch-up under way
 * will still take, is longer than that, it halts trading and leaves a person to decide when it resumes, rather than
 * leaving members' orders unanswered for as long as the recovery happens to take
 * (docs/availability/far_back_catch_up.md, section 5).
 *
 * The time no engine has led is the absence that EngineAbsenceRule measures; this rule is given when it began. How long
 * the catch-up will still take is worked out from how far the reading thread has sent it and how fast that moved over
 * the last second: sending is paced by what the engine's connection takes, so that is the rate the engine applies
 * records. Once a catch-up has run for a second, the rule acts if the time so far and the time still to come together
 * pass the limit, without waiting for the limit to pass; and in any case once the time so far alone has passed it.
 *
 * The halt is due once for each absence, only to the instance that leads, and only while the WAL holds a record:
 * before the first record there is nothing to recover. An absence ends when an engine is routed to again.
 *
 * It decides; it does not log, write or send. One thread, the sequencer's, calls every member function. Times are
 * wall-clock nanoseconds.
 */
class RecoveryLimitRule {
  public:
    /// How far a catch-up has got: it has been sent every record up to sent_through of those up to serving_through.
    struct CatchUpProgress {
        int64_t sent_through{0};
        int64_t serving_through{0};
    };

    explicit RecoveryLimitRule(std::chrono::seconds limit) : limit_ns_{std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count()} {}

    /**
     * @brief Whether the halt is due now. Called about once a second.
     * @param[in] now_ns            The time now.
     * @param[in] absence_began_ns  When the present absence of an engine began, or 0 if an engine is routed to.
     * @param[in] leads             Whether this instance leads, which is the only instance that writes commands.
     * @param[in] wal_holds_records Whether the WAL holds any record.
     * @param[in] progress          How far the catch-up of an engine has got, if one is under way.
     */
    [[nodiscard]] bool halt_due(int64_t now_ns, int64_t absence_began_ns, bool leads, bool wal_holds_records, std::optional<CatchUpProgress> progress) {
        if (absence_began_ns == 0) {
            // An engine is routed to: the absence, and with it what was decided about it, is over.
            halt_written_ = false;
            last_sample_.reset();
            estimate_ns_.reset();
            return false;
        }
        update_estimate(now_ns, progress);
        if (!leads || !wal_holds_records || halt_written_) {
            return false;
        }
        const int64_t so_far_ns = now_ns - absence_began_ns;
        if (so_far_ns > limit_ns_) {
            return true;
        }
        return estimate_ns_.has_value() && so_far_ns + *estimate_ns_ > limit_ns_;
    }

    /// The halt has been written for this absence; it is not written again until an engine has been routed to.
    void halt_written() {
        halt_written_ = true;
    }

    /// How long the catch-up under way is expected still to take, in nanoseconds, if it has run long enough to tell.
    [[nodiscard]] std::optional<int64_t> estimate_ns() const {
        return estimate_ns_;
    }

    /// The limit, in nanoseconds.
    [[nodiscard]] int64_t limit_ns() const {
        return limit_ns_;
    }

  private:
    struct Sample {
        int64_t at_ns{0};
        int64_t sent_through{0};
    };

    // Works out how long the catch-up will still take from the rate over the last second. A sample less than a second
    // old is kept until it is a second old, so that the rate is measured over at least a second.
    void update_estimate(int64_t now_ns, std::optional<CatchUpProgress> progress) {
        if (!progress.has_value()) {
            last_sample_.reset();
            estimate_ns_.reset();
            return;
        }
        if (!last_sample_.has_value() || progress->sent_through < last_sample_->sent_through) {
            // The first sample of a catch-up, or of a new one that started lower.
            last_sample_ = Sample{now_ns, progress->sent_through};
            estimate_ns_.reset();
            return;
        }
        const int64_t elapsed_ns = now_ns - last_sample_->at_ns;
        if (elapsed_ns < one_second_ns) {
            return;
        }
        const int64_t moved = progress->sent_through - last_sample_->sent_through;
        const int64_t remaining = progress->serving_through - progress->sent_through;
        if (moved > 0) {
            // Records still to send, at the rate they moved. In floating point, since remaining times elapsed can be
            // larger than an int64_t holds; an estimate needs no more precision than that.
            const double nanoseconds_per_record = static_cast<double>(elapsed_ns) / static_cast<double>(moved);
            estimate_ns_ = static_cast<int64_t>(static_cast<double>(remaining > 0 ? remaining : 0) * nanoseconds_per_record);
        } else {
            // Nothing moved for a second: no estimate, and the time so far alone decides.
            estimate_ns_.reset();
        }
        last_sample_ = Sample{now_ns, progress->sent_through};
    }

    static constexpr int64_t one_second_ns = 1'000'000'000;

    int64_t limit_ns_;
    bool halt_written_{false};
    std::optional<Sample> last_sample_;
    std::optional<int64_t> estimate_ns_;
};

} // namespaces
