#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>

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
        auto voter = voters_.find(group);
        if (voter == voters_.end()) {
            voter = voters_.emplace(group, fix_common::LeaseVoter(lease_period_, active_since_, highest_epochs_[group])).first;
        }
        const fix_common::LeaseVoter::Answer answer = voter->second.consider(candidate_id, epoch, now);
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

  private:
    Clock::duration lease_period_;
    bool active_{false};
    Clock::time_point active_since_{};
    std::map<pubsub_itc_fw_app::ComponentGroup, fix_common::LeaseVoter> voters_;
    std::map<pubsub_itc_fw_app::ComponentGroup, int32_t> highest_epochs_;
};

} // namespaces
