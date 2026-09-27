#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <LeaderEpoch.hpp>
#include <LeaseHolder.hpp>
#include <LeaseVoter.hpp>

namespace fix_common {

/**
 * @brief One instance of a pair, which both votes and may lead: its voter and its lease holder, and
 *        the rules that join the two.
 *
 * Each instance of a pair is one of the three voters, and is also one of the two that may lead. The
 * rules that concern only one side live in LeaseVoter and LeaseHolder. This class adds the rules that
 * join them, which are the ones model checking showed are easy to get wrong
 * (docs/availability/majority_leases.md, rules 4, 5, 9 and 10):
 *
 *   - An instance that is leading, or asking to lead, votes for itself, and so grants nothing to its peer.
 *   - An instance that has granted its peer a lease does not ask to lead until that lease has run out.
 *   - An instance asking to lead that receives its peer's request at a higher epoch gives up and grants
 *     it. Epochs record which instance leads in them, so the two requests never carry the same epoch,
 *     and exactly one instance gives way. Without this, two instances asking at the same moment with
 *     the third voter down would refuse each other for ever.
 *   - An instance whose request to lead fails waits before asking again. The owner chooses how long,
 *     and should vary it from one attempt to the next.
 *
 * The epoch an instance asks to lead in is the next one above the highest it knows in which it leads
 * (LeaderEpoch::next_for). The highest epoch it knows is what the owner keeps on disk: read it with
 * highest_epoch() after any call and store it when it has risen.
 */
class LeaseParticipant {
  public:
    using Clock = std::chrono::steady_clock;

    /// The answer to a peer's request, and whether answering it ended this instance's own request to lead.
    struct PeerRequestOutcome {
        LeaseVoter::Answer answer;
        bool gave_up_asking_to_lead{false};
    };

    /**
     * @param[in] self_id This instance's id: 1 or 2.
     * @param[in] lease_period How long a grant lasts.
     * @param[in] drift_allowance How much shorter than the lease period this instance takes a lease it holds to be.
     * @param[in] started_at When this instance started.
     * @param[in] persisted_epoch The highest epoch this instance knew of when it last stopped.
     */
    LeaseParticipant(int64_t self_id, Clock::duration lease_period, Clock::duration drift_allowance, Clock::time_point started_at, int32_t persisted_epoch)
        : self_id_(self_id), voter_(lease_period, started_at, persisted_epoch), holder_(lease_period, drift_allowance) {}

    /**
     * @brief The peer asks this instance for a lease.
     * @param[in] candidate_id The peer's id.
     * @param[in] epoch The epoch it asks for.
     * @param[in] now The time on this instance's clock.
     */
    [[nodiscard]] PeerRequestOutcome on_peer_request(int64_t candidate_id, int32_t epoch, Clock::time_point now) {
        PeerRequestOutcome outcome;
        if (holder_.state() == LeaseHolder::State::Candidate && epoch > holder_.epoch() && !voter_.restarting(now)) {
            holder_.stop();
            voter_.release_self();
            outcome.gave_up_asking_to_lead = true;
        }
        outcome.answer = voter_.consider(candidate_id, epoch, now);
        return outcome;
    }

    /**
     * @brief Whether this instance may start asking to lead now.
     *
     * It may not while it leads or already asks, while it started less than a lease period ago, while
     * its vote is promised to its peer, or while it is waiting after a failed attempt.
     */
    [[nodiscard]] bool may_ask_to_lead(Clock::time_point now) const {
        return holder_.state() == LeaseHolder::State::Idle && !voter_.restarting(now) && !voter_.promised_elsewhere(self_id_, now) && now >= wait_until_;
    }

    /**
     * @brief Start asking to lead. The caller then sends a request to each other voter, using record_request() for each.
     * @return The epoch asked for.
     */
    [[nodiscard]] int32_t begin_asking_to_lead() {
        const int32_t epoch = LeaderEpoch::next_for(voter_.highest_epoch(), self_id_);
        holder_.begin_candidacy(epoch);
        voter_.hold_for_self();
        return epoch;
    }

    /// Note that a request, to lead or to renew, is being sent to @p voter_id now; returns the id to put on it.
    [[nodiscard]] int64_t record_request(int64_t voter_id, Clock::time_point now) {
        return holder_.record_request(voter_id, now);
    }

    /**
     * @brief A voter granted a request.
     * @return BecameLeader when this instance now leads; its epoch is then the highest it knows.
     */
    [[nodiscard]] LeaseHolder::Event on_grant(int64_t voter_id, int64_t request_id, int32_t epoch, Clock::time_point now) {
        const LeaseHolder::Event event = holder_.on_grant(voter_id, request_id, epoch, now);
        if (event == LeaseHolder::Event::BecameLeader) {
            voter_.learn_epoch(epoch);
        }
        return event;
    }

    /**
     * @brief A voter refused a request.
     * @param[in] request_id The id the request carried.
     * @param[in] highest_epoch The highest epoch the voter has granted.
     * @param[in] now The time on this instance's clock.
     * @param[in] wait_before_asking_again How long to wait before asking to lead again, if this stops it.
     * @return NewerEpochKnown when the voter knows a newer generation; this instance has then stopped
     *         leading or asking to lead.
     */
    [[nodiscard]] LeaseHolder::Event on_refusal(int64_t request_id, int32_t highest_epoch, Clock::time_point now, Clock::duration wait_before_asking_again) {
        const LeaseHolder::Event event = holder_.on_refusal(request_id, highest_epoch);
        voter_.learn_epoch(highest_epoch);
        if (event == LeaseHolder::Event::NewerEpochKnown) {
            stop(now, wait_before_asking_again);
        }
        return event;
    }

    /**
     * @brief Stop leading or asking to lead, and wait before asking again.
     *
     * Called when a request to lead has gone unanswered for long enough, and by the class itself when
     * a newer generation is learnt of.
     */
    void stop(Clock::time_point now, Clock::duration wait_before_asking_again) {
        holder_.stop();
        voter_.release_self();
        wait_until_ = now + wait_before_asking_again;
    }

    /**
     * @brief If this instance leads but its last lease has run out, stop leading.
     * @return true when it stopped: the caller must stop acting as leader, if it has not already.
     */
    [[nodiscard]] bool stop_if_lease_ran_out(Clock::time_point now) {
        if (holder_.state() != LeaseHolder::State::Leading || holder_.acting(now)) {
            return false;
        }
        holder_.stop();
        voter_.release_self();
        return true;
    }

    /// Whether this instance leads and holds an unexpired lease, and so may act as leader.
    [[nodiscard]] bool acting(Clock::time_point now) const {
        return holder_.acting(now);
    }

    [[nodiscard]] LeaseHolder::State state() const {
        return holder_.state();
    }

    /// The epoch this instance leads in, or asks to lead in.
    [[nodiscard]] int32_t epoch() const {
        return holder_.epoch();
    }

    /// Resume with the promise recorded before this instance restarted, instead of waiting. See LeaseVoter.
    void resume_with_kept_promise(int64_t promised_to, Clock::time_point until, Clock::time_point now) {
        voter_.resume_with_kept_promise(promised_to, until, now);
    }

    /// The peer this instance's vote is promised to, or zero, as last granted.
    [[nodiscard]] int64_t promised_to() const {
        return voter_.promised_to();
    }

    /// When this instance's promise as a voter runs out.
    [[nodiscard]] Clock::time_point promise_expires_at() const {
        return voter_.promise_expires_at();
    }

    /// The highest epoch this instance has granted, led in, or learnt of. The owner keeps this on disk.
    [[nodiscard]] int32_t highest_epoch() const {
        return voter_.highest_epoch();
    }

    /// When this instance's latest lease runs out, as it counts it.
    [[nodiscard]] Clock::time_point lease_expires_at() const {
        return holder_.lease_expires_at();
    }

  private:
    int64_t self_id_;
    LeaseVoter voter_;
    LeaseHolder holder_;
    Clock::time_point wait_until_{};
};

} // namespaces
