#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>

#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LeaseVoter.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The arbiter pool's vote in deciding which instance of each component pair leads.
 *
 * For a component pair the three voters are the two instances and the arbiter pool. The pool votes
 * through whichever arbiter is active, and only while it is active. This class holds that vote, one
 * LeaseVoter per pair, each named by the number its instances put on their lease messages.
 *
 * An arbiter that becomes active knows nothing of what the previously active arbiter promised, so
 * it must treat itself exactly as a voter that has just restarted: it grants nothing for one lease
 * period (docs/availability/majority_leases.md, rule 6). That is why the voters are created afresh,
 * starting at the moment this arbiter became active, and discarded when it stops being active.
 * During that period each component leader renews with its peer alone.
 *
 * What does carry across is the highest epoch granted in each pair. The active arbiter copies it to
 * its peer, so that a change of active arbiter does not forget it. A voter that forgets it can grant
 * a new leader an epoch below one already led in, which receivers then ignore until the leader learns
 * of the higher one (docs/availability/tla/traces/lease-5-epoch-regresses.txt). Carrying it narrows
 * that to the case where both arbiters have restarted.
 *
 * The newest statement each pair's leader has made about whether its peer may lead carries across
 * too, and must: unlike a promise, a statement does not run out, and forgetting one could let an
 * instance lacking commands its leader acted on be elected
 * (docs/availability/a_follower_behind_does_not_lead.md). The owner keeps statements on disk and copies
 * them to the other arbiter, and passes back in those it reads or is sent with learn_statement().
 */
class ComponentLeaseVoters {
  public:
    using Clock = std::chrono::steady_clock;

    explicit ComponentLeaseVoters(Clock::duration lease_period) : lease_period_(lease_period) {}

    /// This arbiter has become active at @p now. It grants nothing for one lease period from now.
    void became_active(Clock::time_point now) {
        active_ = true;
        active_since_ = now;
        voters_.clear();
    }

    /// This arbiter is no longer active. Everything it promised is forgotten; the next arbiter to become active waits it out.
    void became_passive() {
        active_ = false;
        voters_.clear();
    }

    [[nodiscard]] bool active() const {
        return active_;
    }

    /**
     * @brief Decide a component instance's request for a lease.
     *
     * Only an active arbiter may answer. A passive one stays silent, because a refusal from it would
     * cancel the request the active arbiter is answering with the same id.
     */
    [[nodiscard]] pubsub_itc_fw::LeaseVoter::Answer consider(int32_t pair_id, int64_t candidate_id, int32_t epoch, Clock::time_point now) {
        if (!active_) {
            throw pubsub_itc_fw::PreconditionAssertion("ComponentLeaseVoters: only an active arbiter answers a component's request for a lease", __FILE__,
                                                       __LINE__);
        }
        pubsub_itc_fw::LeaseVoter& voter = voter_for(pair_id);
        const pubsub_itc_fw::LeaseVoter::Answer answer = voter.consider(candidate_id, epoch, now);
        learn_epoch(pair_id, answer.highest_epoch);
        return answer;
    }

    /// Record an epoch granted in @p pair_id, whether by this arbiter or, as its peer reports, by the other one.
    void learn_epoch(int32_t pair_id, int32_t epoch) {
        int32_t& highest = highest_epochs_[pair_id];
        if (epoch > highest) {
            highest = epoch;
        }
        const auto voter = voters_.find(pair_id);
        if (voter != voters_.end()) {
            voter->second.learn_epoch(epoch);
        }
    }

    /// The highest epoch known to have been granted in @p pair_id.
    [[nodiscard]] int32_t highest_epoch(int32_t pair_id) const {
        const auto found = highest_epochs_.find(pair_id);
        return found == highest_epochs_.end() ? 0 : found->second;
    }

    /// Every pair with a known highest epoch, to replay to a peer that has just connected.
    [[nodiscard]] const std::map<int32_t, int32_t>& highest_epochs() const {
        return highest_epochs_;
    }

    /**
     * @brief Record the statement carried on a request this arbiter has just granted.
     *
     * Only a statement newer than the one held for the pair replaces it. The owner must make the
     * statements durable before echoing the number on the grant.
     *
     * @return The number to echo on the grant if the arbiter now holds the statement, or zero.
     */
    [[nodiscard]] int64_t record_statement(int32_t pair_id, const pubsub_itc_fw::LeaderStatement& statement) {
        pubsub_itc_fw::LeaseVoter& voter = voter_for(pair_id);
        voter.record_statement(statement);
        statements_[pair_id] = voter.recorded_statement();
        return voter.holds_statement(statement) ? statement.number : 0;
    }

    /**
     * @brief Take a statement read from disk or sent by the other arbiter, if it is newer than the one held.
     * @return true when it replaced the one held.
     */
    bool learn_statement(int32_t pair_id, const pubsub_itc_fw::LeaderStatement& statement) {
        pubsub_itc_fw::LeaderStatement& held = statements_[pair_id];
        if (!statement.newer_than(held)) {
            return false;
        }
        held = statement;
        const auto voter = voters_.find(pair_id);
        if (voter != voters_.end()) {
            voter->second.record_statement(statement);
        }
        return true;
    }

    /// The newest statement held for @p pair_id; its leader_id is zero if none is held.
    [[nodiscard]] pubsub_itc_fw::LeaderStatement statement(int32_t pair_id) const {
        const auto found = statements_.find(pair_id);
        return found == statements_.end() ? pubsub_itc_fw::LeaderStatement{} : found->second;
    }

    /// Every statement held, to keep on disk and to replay to a peer that has just connected.
    [[nodiscard]] const std::map<int32_t, pubsub_itc_fw::LeaderStatement>& statements() const {
        return statements_;
    }

  private:
    // The voter for @p pair_id, created when first needed with the highest epoch and the statement held.
    pubsub_itc_fw::LeaseVoter& voter_for(int32_t pair_id) {
        auto voter = voters_.find(pair_id);
        if (voter == voters_.end()) {
            voter = voters_.emplace(pair_id, pubsub_itc_fw::LeaseVoter(lease_period_, active_since_, highest_epochs_[pair_id])).first;
            const auto statement = statements_.find(pair_id);
            if (statement != statements_.end()) {
                voter->second.restore_recorded_statement(statement->second);
            }
        }
        return voter->second;
    }

    Clock::duration lease_period_;
    bool active_{false};
    Clock::time_point active_since_{};
    std::map<int32_t, pubsub_itc_fw::LeaseVoter> voters_;
    std::map<int32_t, int32_t> highest_epochs_;
    std::map<int32_t, pubsub_itc_fw::LeaderStatement> statements_;
};

} // namespaces
