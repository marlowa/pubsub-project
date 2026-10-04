#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>

#include <LeaderStatement.hpp>
#include <LeaseVoter.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace arbiter {

/**
 * @brief The arbiter pool's vote in deciding which instance of each component pair leads.
 *
 * For a component pair the three voters are the two instances and the arbiter pool. The pool votes
 * through whichever arbiter is active, and only while it is active. This class holds that vote, one
 * LeaseVoter per component group.
 *
 * An arbiter that becomes active knows nothing of what the previously active arbiter promised, so
 * it must treat itself exactly as a voter that has just restarted: it grants nothing for one lease
 * period (docs/availability/majority_leases.md, rule 6). That is why the voters are created afresh,
 * starting at the moment this arbiter became active, and discarded when it stops being active.
 * During that period each component leader renews with its peer alone.
 *
 * What does carry across is the highest epoch granted in each group. The active arbiter copies it to
 * its peer, so that a change of active arbiter does not forget it. A voter that forgets it can grant
 * a new leader an epoch below one already led in, which receivers then ignore until the leader learns
 * of the higher one (docs/availability/tla/traces/lease-5-epoch-regresses.txt). Carrying it narrows
 * that to the case where both arbiters have restarted.
 *
 * The newest statement each group's leader has made about whether its peer may lead carries across
 * too, and must: unlike a promise, a statement does not run out, and forgetting one could let an
 * instance lacking commands the matching engine acted on be elected
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
    [[nodiscard]] fix_common::LeaseVoter::Answer consider(pubsub_itc_fw_app::ComponentGroup group, int64_t candidate_id, int32_t epoch, Clock::time_point now) {
        if (!active_) {
            throw pubsub_itc_fw::PreconditionAssertion("ComponentLeaseVoters: only an active arbiter answers a component's request for a lease", __FILE__,
                                                       __LINE__);
        }
        fix_common::LeaseVoter& voter = voter_for(group);
        const fix_common::LeaseVoter::Answer answer = voter.consider(candidate_id, epoch, now);
        learn_epoch(group, answer.highest_epoch);
        return answer;
    }

    /// Record an epoch granted in @p group, whether by this arbiter or, as its peer reports, by the other one.
    void learn_epoch(pubsub_itc_fw_app::ComponentGroup group, int32_t epoch) {
        int32_t& highest = highest_epochs_[group];
        if (epoch > highest) {
            highest = epoch;
        }
        const auto voter = voters_.find(group);
        if (voter != voters_.end()) {
            voter->second.learn_epoch(epoch);
        }
    }

    /// The highest epoch known to have been granted in @p group.
    [[nodiscard]] int32_t highest_epoch(pubsub_itc_fw_app::ComponentGroup group) const {
        const auto found = highest_epochs_.find(group);
        return found == highest_epochs_.end() ? 0 : found->second;
    }

    /// Every group with a known highest epoch, to replay to a peer that has just connected.
    [[nodiscard]] const std::map<pubsub_itc_fw_app::ComponentGroup, int32_t>& highest_epochs() const {
        return highest_epochs_;
    }

    /**
     * @brief Record the statement carried on a request this arbiter has just granted.
     *
     * Only a statement newer than the one held for the group replaces it. The owner must make the
     * statements durable before echoing the number on the grant.
     *
     * @return The number to echo on the grant if the arbiter now holds the statement, or zero.
     */
    [[nodiscard]] int64_t record_statement(pubsub_itc_fw_app::ComponentGroup group, const fix_common::LeaderStatement& statement) {
        fix_common::LeaseVoter& voter = voter_for(group);
        voter.record_statement(statement);
        statements_[group] = voter.recorded_statement();
        return voter.holds_statement(statement) ? statement.number : 0;
    }

    /**
     * @brief Take a statement read from disk or sent by the other arbiter, if it is newer than the one held.
     * @return true when it replaced the one held.
     */
    bool learn_statement(pubsub_itc_fw_app::ComponentGroup group, const fix_common::LeaderStatement& statement) {
        fix_common::LeaderStatement& held = statements_[group];
        if (!statement.newer_than(held)) {
            return false;
        }
        held = statement;
        const auto voter = voters_.find(group);
        if (voter != voters_.end()) {
            voter->second.record_statement(statement);
        }
        return true;
    }

    /// The newest statement held for @p group; its leader_id is zero if none is held.
    [[nodiscard]] fix_common::LeaderStatement statement(pubsub_itc_fw_app::ComponentGroup group) const {
        const auto found = statements_.find(group);
        return found == statements_.end() ? fix_common::LeaderStatement{} : found->second;
    }

    /// Every statement held, to keep on disk and to replay to a peer that has just connected.
    [[nodiscard]] const std::map<pubsub_itc_fw_app::ComponentGroup, fix_common::LeaderStatement>& statements() const {
        return statements_;
    }

  private:
    // The voter for @p group, created when first needed with the highest epoch and the statement held.
    fix_common::LeaseVoter& voter_for(pubsub_itc_fw_app::ComponentGroup group) {
        auto voter = voters_.find(group);
        if (voter == voters_.end()) {
            voter = voters_.emplace(group, fix_common::LeaseVoter(lease_period_, active_since_, highest_epochs_[group])).first;
            const auto statement = statements_.find(group);
            if (statement != statements_.end()) {
                voter->second.restore_recorded_statement(statement->second);
            }
        }
        return voter->second;
    }

    Clock::duration lease_period_;
    bool active_{false};
    Clock::time_point active_since_{};
    std::map<pubsub_itc_fw_app::ComponentGroup, fix_common::LeaseVoter> voters_;
    std::map<pubsub_itc_fw_app::ComponentGroup, int32_t> highest_epochs_;
    std::map<pubsub_itc_fw_app::ComponentGroup, fix_common::LeaderStatement> statements_;
};

} // namespaces
