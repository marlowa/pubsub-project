#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief How a new leadership epoch is chosen, so that an epoch names exactly one leader.
 *
 * The epoch is the generation counter that every receiver checks on every PDU. A receiver accepts
 * an epoch equal to the one it holds, so if two different instances ever lead at the same epoch,
 * both are believed, and the check cannot tell them apart.
 *
 * Each instance of a pair chooses the epoch it asks to lead in, from the highest it knows, and both
 * may ask from the same known epoch at the same moment. If each took "the highest epoch it knows,
 * plus one", both would ask in the same number, and nothing in the lease rules would stop a voter
 * granting it to one and later to the other.
 *
 * So the value of an epoch records which instance leads in it: its remainder on division by
 * epoch_stride is that instance's id. Every instance that asks to lead takes the next such number
 * above what it knows. Two different instances then never lead at the same epoch, however their
 * requests are timed. The model of the lease rules checks this as the property EpochNamesOneLeader
 * (docs/availability/tla/findings.md, section 2). Ordinary
 * integer comparison still orders generations correctly, because the generation is the quotient
 * and dominates the remainder.
 *
 * The same rule serves the two arbiters, whose epochs record which arbiter is active.
 */
class LeaderEpoch {
  public:
    /// Room for the instance ids in use, which are 1 (primary) and 2 (secondary).
    static constexpr int32_t epoch_stride = 4;

    /**
     * @brief The smallest epoch above a known one in which a given instance leads.
     * @param[in] above An epoch already known; the result is strictly greater than it.
     * @param[in] leader_instance_id The instance that is to lead: 1 or 2.
     * @return The next epoch, above @p above, whose remainder on division by epoch_stride is
     *         @p leader_instance_id.
     */
    [[nodiscard]] static int32_t next_for(int32_t above, int64_t leader_instance_id) {
        if (leader_instance_id < 1 || leader_instance_id >= epoch_stride) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LeaderEpoch::next_for: instance id {} is not in [1, {})", leader_instance_id, epoch_stride),
                                                       __FILE__, __LINE__);
        }
        if (above < 0 || above > std::numeric_limits<int32_t>::max() - 2 * epoch_stride) {
            throw pubsub_itc_fw::PreconditionAssertion(fmt::format("LeaderEpoch::next_for: epoch {} is out of range", above), __FILE__, __LINE__);
        }
        const int32_t leader = static_cast<int32_t>(leader_instance_id);
        const int32_t base = (above / epoch_stride) * epoch_stride;
        return base + leader > above ? base + leader : base + epoch_stride + leader;
    }
};

} // namespaces
