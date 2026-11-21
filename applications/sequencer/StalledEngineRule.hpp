#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <optional>

namespace sequencer {

/**
 * @brief Decides whether a matching engine that stays connected has stopped working: stopped applying the commands it
 *        is sent, or stopped sending the reports of those it has applied.
 *
 * A connection, a lease and a heartbeat show that an engine can communicate, not that it works. The evidence of work is
 * its progress, which the sequencer asks for once a second with EnginePositionQuery. Each query and the answer to it are
 * a round. Starting a round judges the round just ended and then sends a new query
 * (docs/availability/a_matching_engine_that_stops_working.md, section 3). Two times are kept:
 *
 * - the time without applying a command: it runs while the engine has been sent a command it has not applied and its
 *   highest applied command has not changed since the previous answer;
 * - the time without sending a report: it runs while the engine holds reports and the last report it sent, the pair of
 *   epoch and number, has not changed since the previous answer. The pair is compared, because a report's number starts
 *   again under each new epoch.
 *
 * A round whose answer does not arrive counts as no movement, and an answer counts only for the round whose request id
 * it carries, so a late answer to an earlier query can neither show progress nor hide its absence. The engine is judged
 * by time without progress, not by how far behind it is: under overload a healthy engine is tens of thousands of
 * commands behind, and still moves.
 *
 * While the sequencer has messages from the engine it has not yet taken, both times start again from zero: the engine
 * may be paused waiting for the sequencer to read its reports, and the delay is then the sequencer's, not the engine's.
 *
 * A time that starts runs from the moment the round being judged began, which is the latest moment at which the engine
 * can be shown to have moved, or from the moment the sequencer was first seen with nothing unread from the engine, if
 * that is later. So the engine is judged stopped between the limit and the limit plus one round after it stops, and
 * never sooner than the limit after the sequencer has caught up with it.
 *
 * It decides; it does not log, write or send. One thread, the sequencer's, calls every member function. Times are
 * nanoseconds on one clock, whichever the caller uses.
 */
class StalledEngineRule {
  public:
    /// What the engine said in EnginePosition.
    struct Position {
        int64_t highest_applied{0};
        int32_t reports_sent_epoch{0};
        int64_t reports_sent_through{0};
        int64_t reports_held{0};
    };

    /// The judgement made at the start of a round.
    enum class Verdict { working, stopped_applying, stopped_sending };

    explicit StalledEngineRule(std::chrono::seconds limit) : limit_ns_{std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count()} {}

    /**
     * @brief Judges the round just ended, then starts a new one. Called once a second, just before the query is sent.
     * @param[in] now_ns             The time now.
     * @param[in] highest_sent       The highest sequence number of a command sent to the engine so far.
     * @param[in] unread_from_engine Whether the sequencer has, now, messages from the engine it has not yet taken.
     * @return The judgement of the engine as of the round just ended.
     */
    [[nodiscard]] Verdict start_round(int64_t now_ns, int64_t highest_sent, bool unread_from_engine) {
        if (round_open_) {
            judge_round(now_ns, unread_from_engine);
        }
        round_open_ = true;
        ++request_id_;
        round_started_ns_ = now_ns;
        round_highest_sent_ = highest_sent;
        round_answer_.reset();
        return verdict(now_ns);
    }

    /// The request id to send on the query of the round just started.
    [[nodiscard]] int64_t request_id() const {
        return request_id_;
    }

    /**
     * @brief The engine's answer to a query. Ignored unless it answers the query of the present round.
     * @param[in] request_id The request id the answer carries.
     * @param[in] position   What the engine said.
     */
    void answer(int64_t request_id, const Position& position) {
        if (!round_open_ || request_id != request_id_) {
            return;
        }
        round_answer_ = position;
    }

    /// Forgets everything about the engine, for when the sequencer stops judging it or starts judging another.
    void restart() {
        round_open_ = false;
        round_answer_.reset();
        last_answer_.reset();
        applying_since_ns_.reset();
        sending_since_ns_.reset();
        nothing_unread_since_ns_.reset();
    }

    /// The longest either time has run, in nanoseconds, since the rule was made: the longest an engine has been seen
    /// without progress while it had work.
    [[nodiscard]] int64_t longest_without_progress_ns() const {
        return longest_without_progress_ns_;
    }

    /// The limit, in nanoseconds.
    [[nodiscard]] int64_t limit_ns() const {
        return limit_ns_;
    }

  private:
    void judge_round(int64_t now_ns, bool unread_from_engine) {
        const std::optional<Position> previous = last_answer_;
        if (round_answer_.has_value()) {
            last_answer_ = round_answer_;
        }
        if (unread_from_engine) {
            stop(applying_since_ns_, now_ns);
            stop(sending_since_ns_, now_ns);
            nothing_unread_since_ns_.reset();
            return;
        }
        if (!nothing_unread_since_ns_.has_value()) {
            nothing_unread_since_ns_ = now_ns;
        }
        judge_applying(now_ns, previous);
        judge_sending(now_ns, previous);
    }

    void judge_applying(int64_t now_ns, const std::optional<Position>& previous) {
        if (round_answer_.has_value()) {
            const bool moved = !previous.has_value() || round_answer_->highest_applied != previous->highest_applied;
            const bool work_outstanding = round_highest_sent_ > round_answer_->highest_applied;
            if (moved || !work_outstanding) {
                stop(applying_since_ns_, now_ns);
                return;
            }
        } else {
            const int64_t applied_known = previous.has_value() ? previous->highest_applied : 0;
            if (round_highest_sent_ <= applied_known) {
                stop(applying_since_ns_, now_ns);
                return;
            }
        }
        run(applying_since_ns_);
    }

    void judge_sending(int64_t now_ns, const std::optional<Position>& previous) {
        if (round_answer_.has_value()) {
            const bool moved = !previous.has_value() || round_answer_->reports_sent_epoch != previous->reports_sent_epoch ||
                               round_answer_->reports_sent_through != previous->reports_sent_through;
            if (moved || round_answer_->reports_held == 0) {
                stop(sending_since_ns_, now_ns);
                return;
            }
        } else if (!previous.has_value() || previous->reports_held == 0) {
            stop(sending_since_ns_, now_ns);
            return;
        }
        run(sending_since_ns_);
    }

    // A time that starts runs from the start of the round being judged, or from when the sequencer was first seen with
    // nothing unread from the engine, if that is later.
    void run(std::optional<int64_t>& since_ns) const {
        if (!since_ns.has_value()) {
            const int64_t caught_up_ns = nothing_unread_since_ns_.value_or(0);
            since_ns = round_started_ns_ > caught_up_ns ? round_started_ns_ : caught_up_ns;
        }
    }

    // A time that stops is recorded, if it is the longest seen, and starts again from zero.
    void stop(std::optional<int64_t>& since_ns, int64_t now_ns) {
        if (since_ns.has_value()) {
            note_longest(now_ns - *since_ns);
        }
        since_ns.reset();
    }

    void note_longest(int64_t elapsed_ns) {
        if (elapsed_ns > longest_without_progress_ns_) {
            longest_without_progress_ns_ = elapsed_ns;
        }
    }

    [[nodiscard]] Verdict verdict(int64_t now_ns) {
        if (applying_since_ns_.has_value()) {
            note_longest(now_ns - *applying_since_ns_);
            if (now_ns - *applying_since_ns_ >= limit_ns_) {
                return Verdict::stopped_applying;
            }
        }
        if (sending_since_ns_.has_value()) {
            note_longest(now_ns - *sending_since_ns_);
            if (now_ns - *sending_since_ns_ >= limit_ns_) {
                return Verdict::stopped_sending;
            }
        }
        return Verdict::working;
    }

    int64_t limit_ns_;
    bool round_open_{false};
    int64_t request_id_{0};
    int64_t round_started_ns_{0};
    int64_t round_highest_sent_{0};
    std::optional<Position> round_answer_;
    std::optional<Position> last_answer_;
    std::optional<int64_t> applying_since_ns_;
    std::optional<int64_t> sending_since_ns_;
    std::optional<int64_t> nothing_unread_since_ns_;
    int64_t longest_without_progress_ns_{0};
};

} // namespaces
