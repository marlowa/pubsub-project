#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace fix_common {

/**
 * @brief What a leader says about its peer: whether the peer may lead.
 *
 * A leader normally has the matching engine act on a command only once its peer holds it. Before it
 * acts on a command its peer does not hold, a voter other than itself must have recorded that the
 * peer may not lead, so that no majority can elect an instance lacking a command the engine acted on.
 * The leader says this on every lease request it sends, and records it itself. Every leadership
 * starts by saying that the peer may not lead, and says that it may only once the peer holds every
 * command the leader holds. See docs/availability/a_follower_behind_does_not_lead.md, and rule 11 in
 * docs/availability/tla/FollowerBehindHA.tla, which shows each part is needed.
 *
 * Statements are ordered by epoch and then by number, so that a statement arriving late cannot
 * replace a newer one. A voter keeps only the newest it has been given.
 */
struct LeaderStatement {
    int64_t leader_id{0};     ///< the instance that made the statement, 1 or 2; zero when there is none
    int32_t epoch{0};         ///< the epoch of the leadership that made it
    int64_t number{0};        ///< goes up by one each time that leadership changes what it says
    bool peer_may_lead{true}; ///< false: the leader's peer may not lead

    /// Whether this statement is newer than @p other: a later epoch, or the same epoch and a higher number.
    [[nodiscard]] bool newer_than(const LeaderStatement& other) const {
        return epoch > other.epoch || (epoch == other.epoch && number > other.number);
    }

    /// Whether this is the same statement as @p other: the same leader, epoch and number.
    [[nodiscard]] bool same_as(const LeaderStatement& other) const {
        return leader_id == other.leader_id && epoch == other.epoch && number == other.number;
    }

    /// The instance this statement says may not lead, or zero if it says none.
    [[nodiscard]] int64_t instance_that_may_not_lead() const {
        if (leader_id == 0 || peer_may_lead) {
            return 0;
        }
        return leader_id == 1 ? 2 : 1;
    }
};

} // namespaces
