#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>

namespace arbiter {

/**
 * @brief Which of a component pair should lead, and under which epoch.
 *
 * Pure: it reads no state and sends nothing, so the rule can be tested directly rather than
 * through a running venue. ArbiterThread gathers the inputs from its connection tables and
 * carries out the decision.
 *
 * **Two questions, not one**
 *
 * "Which instance should lead when neither leads yet?" and "which should lead when one of them
 * already does?" are different questions, and answering them with a single rule is what this
 * class exists to stop.
 *
 * The first is a **cold start**. The two instances come up in either order, possibly seconds
 * apart, and something has to make the outcome the same regardless. Preferring the lower
 * instance id settles it, and the primary always holds the lower one -- see
 * docs/availability/design_notes.md.
 *
 * The second is a **rejoin**, and there the preference is wrong. An instance that restarts has
 * lost whatever state it held, because losing it is why it restarted. Handing leadership back
 * to it on the strength of its id moves the venue from an instance holding a populated order
 * book to one holding nothing. So an instance that is already leading keeps leading, provided
 * it is still there to lead.
 *
 * **The epoch moves only when leadership does**
 *
 * An arbitration that confirms the incumbent returns the epoch already in force. Bumping it
 * would invalidate the leader's own view of itself for no reason: epochs are checked on every
 * PDU, so a leader carrying an epoch the arbiter has just superseded has its traffic discarded
 * while it is doing nothing wrong. The epoch advances when the answer changes, and not
 * otherwise.
 *
 * When it does advance it is taken from the higher of the arbiter's record and the reporter's,
 * so a component that has been away and comes back with a stale epoch cannot wind the sequence
 * backwards.
 */
struct LeadershipDecision {
    /// What the arbiter knows at the moment it is asked.
    struct Inputs {
        /// The instance that sent the ArbitrationReport.
        int64_t self_instance_id{0};
        /// Its peer, as named in that report.
        int64_t peer_instance_id{0};
        /// Whether the peer currently holds a connection to this arbiter.
        bool peer_connected{false};
        /// Whether any leader is on record for this group at all.
        bool has_incumbent{false};
        /// The instance on record as leader. Meaningless when has_incumbent is false.
        int64_t incumbent_instance_id{0};
        /// Whether that instance currently holds a connection to this arbiter.
        bool incumbent_connected{false};
        /// The epoch of the decision on record.
        int32_t incumbent_epoch{0};
        /// The epoch the reporting instance believes is in force.
        int32_t reported_epoch{0};
    };

    int64_t leader_instance_id{0};
    int64_t follower_instance_id{0};
    int32_t epoch{0};
    /// False when the incumbent was confirmed, so the caller can log the two cases apart.
    bool leadership_changed{false};

    [[nodiscard]] static LeadershipDecision decide(const Inputs& inputs) {
        LeadershipDecision decision;

        // A leader that is still connected keeps leadership, whoever asked and whatever the
        // ids are. This is the rejoin case: the instance asking may be one that has just
        // restarted, and it must not displace a peer that has been serving in its absence.
        if (inputs.has_incumbent && inputs.incumbent_connected) {
            decision.leader_instance_id = inputs.incumbent_instance_id;
            decision.epoch = inputs.incumbent_epoch;
            decision.leadership_changed = false;
        } else {
            // Cold start, or the incumbent has gone. Prefer the lower id, but only when the
            // peer is actually there: with no peer connected the reporter is the only
            // candidate and takes leadership whatever its id.
            decision.leader_instance_id = inputs.peer_connected ? std::min(inputs.self_instance_id, inputs.peer_instance_id) : inputs.self_instance_id;
            decision.epoch = std::max(inputs.incumbent_epoch, inputs.reported_epoch) + 1;
            decision.leadership_changed = true;
        }

        // The follower is whichever of the pair is not leading. Derived rather than passed in,
        // so the two can never disagree.
        decision.follower_instance_id = (decision.leader_instance_id == inputs.self_instance_id) ? inputs.peer_instance_id : inputs.self_instance_id;
        return decision;
    }
};

} // namespaces
