#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace fix_common {

/**
 * @brief One voter's side of deciding which instance of a pair leads: the promises it has made.
 *
 * An instance of a pair may lead only while a majority of three voters has granted it a lease that
 * has not run out. For a component pair the voters are the two instances and the active arbiter; for
 * the arbiters they are the two arbiters and the witness. The design, and the model checking that
 * shows these rules are needed, are in docs/availability/majority_leases.md.
 *
 * This class applies the rules a voter follows, and nothing else:
 *
 *   - It grants a lease to at most one instance at a time. Granting one is a promise not to grant a
 *     lease to anyone else until the lease period has passed, counted from the moment of granting.
 *   - It grants nothing for one lease period after it starts, because it has forgotten what it
 *     promised before it stopped.
 *   - It never grants an epoch lower than the highest it has granted.
 *   - While the instance it belongs to is leading, or asking to lead, it has voted for that instance
 *     and grants nothing to anyone else.
 *
 * Time is passed in rather than read, so that the rules can be tested without waiting.
 */
class LeaseVoter {
  public:
    using Clock = std::chrono::steady_clock;

    /// What a voter did with a request.
    enum class Verdict {
        Granted,
        RefusedWhileRestarting,   ///< it started less than one lease period ago
        RefusedPromisedElsewhere, ///< its vote is promised to another instance, or to its own
        RefusedEpochBehind        ///< the request's epoch is below one it has already granted
    };

    /// The answer to one request.
    struct Answer {
        Verdict verdict{Verdict::RefusedWhileRestarting};
        int32_t highest_epoch{0}; ///< the highest epoch this voter has granted, after answering
    };

    /**
     * @param[in] lease_period How long a grant lasts, and how long a voter that has just started grants nothing.
     * @param[in] started_at When this voter started.
     * @param[in] highest_epoch_granted The highest epoch granted before this voter started: what an
     *            instance keeps on disk, or zero for a voter that keeps nothing.
     */
    LeaseVoter(Clock::duration lease_period, Clock::time_point started_at, int32_t highest_epoch_granted)
        : lease_period_(lease_period), quiet_until_(started_at + lease_period), highest_epoch_(highest_epoch_granted) {
        if (lease_period <= Clock::duration::zero()) {
            throw pubsub_itc_fw::PreconditionAssertion("LeaseVoter: the lease period must be positive", __FILE__, __LINE__);
        }
        if (highest_epoch_granted < 0) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LeaseVoter: epoch {} is negative", highest_epoch_granted), __FILE__, __LINE__);
        }
    }

    /**
     * @brief Decide a request for a lease, and record the promise if it is granted.
     * @param[in] candidate_id The instance asking.
     * @param[in] epoch The epoch it asks to lead in.
     * @param[in] now The time on this voter's clock.
     * @return Whether it was granted, and the highest epoch this voter has now granted.
     */
    [[nodiscard]] Answer consider(int64_t candidate_id, int32_t epoch, Clock::time_point now) {
        if (restarting(now)) {
            return Answer{Verdict::RefusedWhileRestarting, highest_epoch_};
        }
        if (held_for_self_ || (promise_live(now) && promised_to_ != candidate_id)) {
            return Answer{Verdict::RefusedPromisedElsewhere, highest_epoch_};
        }
        if (epoch < highest_epoch_) {
            return Answer{Verdict::RefusedEpochBehind, highest_epoch_};
        }
        promised_to_ = candidate_id;
        promise_expires_ = now + lease_period_;
        highest_epoch_ = epoch;
        return Answer{Verdict::Granted, highest_epoch_};
    }

    /**
     * @brief Vote for the instance this voter belongs to, which is leading or asking to lead.
     *
     * Held until release_self(), not for a lease period: it lasts exactly as long as the instance
     * relies on it.
     */
    void hold_for_self() {
        held_for_self_ = true;
    }

    /// Stop voting for the instance this voter belongs to. Safe because only that instance used the vote.
    void release_self() {
        held_for_self_ = false;
    }

    /**
     * @brief Whether this voter's vote is promised to an instance other than @p self_id right now.
     *
     * An instance whose vote is promised to its peer must not ask to lead until the promise runs out,
     * or it would count on a majority while its peer counted on the same vote.
     */
    [[nodiscard]] bool promised_elsewhere(int64_t self_id, Clock::time_point now) const {
        return promise_live(now) && promised_to_ != self_id;
    }

    /// Whether this voter started less than one lease period ago, and so grants nothing.
    [[nodiscard]] bool restarting(Clock::time_point now) const {
        return now < quiet_until_;
    }

    /**
     * @brief Resume with the promise this voter recorded before it restarted, instead of waiting.
     *
     * A voter waits one lease period after starting only because it has forgotten what it promised.
     * One that recorded its promise durably has not forgotten, so it takes the recorded promise back
     * and stops waiting. The promise goes on running out from its recorded expiry, however long the
     * voter was down.
     *
     * @param[in] promised_to The instance the recorded promise was made to, or zero for none.
     * @param[in] until When the recorded promise runs out.
     * @param[in] now The time on this voter's clock.
     */
    void resume_with_kept_promise(int64_t promised_to, Clock::time_point until, Clock::time_point now) {
        quiet_until_ = now;
        promised_to_ = promised_to;
        promise_expires_ = until;
    }

    /// The instance this voter's vote is promised to, or zero. Meaningful only while promise_expires_at() is in the future.
    [[nodiscard]] int64_t promised_to() const {
        return promised_to_;
    }

    /// When this voter's current promise runs out.
    [[nodiscard]] Clock::time_point promise_expires_at() const {
        return promise_expires_;
    }

    /// The highest epoch this voter has granted or learnt of.
    [[nodiscard]] int32_t highest_epoch() const {
        return highest_epoch_;
    }

    /**
     * @brief Record an epoch learnt from elsewhere, such as a refusal, so that nothing below it is granted.
     *
     * Raising the epoch only makes a voter stricter, so this is safe whatever the source.
     */
    void learn_epoch(int32_t epoch) {
        if (epoch > highest_epoch_) {
            highest_epoch_ = epoch;
        }
    }

  private:
    [[nodiscard]] bool promise_live(Clock::time_point now) const {
        return promised_to_ != 0 && now < promise_expires_;
    }

    Clock::duration lease_period_;
    Clock::time_point quiet_until_;
    int64_t promised_to_{0};
    Clock::time_point promise_expires_{};
    bool held_for_self_{false};
    int32_t highest_epoch_{0};
};

} // namespaces
