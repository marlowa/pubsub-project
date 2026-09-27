#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>

#include <fmt/format.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace fix_common {

/**
 * @brief How a new leadership epoch is chosen, so that an epoch names exactly one leader.
 *
 * The epoch is the generation counter that every receiver checks on every PDU. A receiver accepts
 * an epoch equal to the one it holds, so if two different instances ever lead at the same epoch,
 * both are believed, and the check cannot tell them apart.
 *
 * Several parties start generations: the two peers resolving leadership between themselves, the
 * arbiter, and an instance promoting itself when no arbiter can be reached. If each of them took
 * "the highest epoch it knows, plus one", two of them working from the same known epoch would issue
 * the same number for different leaders. Model checking the high availability design showed this
 * happening (docs/availability/tla/findings.md, finding 1).
 *
 * So the value of an epoch records which instance leads in it: its remainder on division by
 * epoch_stride is that instance's id. Every party that starts a generation takes the next such
 * number above what it knows, for the instance it is making leader. Two different instances then
 * never lead at the same epoch, whoever issued it and however the issuers' timing falls. Ordinary
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
