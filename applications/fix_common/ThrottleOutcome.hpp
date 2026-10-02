#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace fix_common {

/**
 * @brief What a session's throttle decided about one command.
 *
 * Besides accepted or refused, the outcome says when a run of refusals begins and ends, because a
 * gateway logs those two moments rather than every refused command: a member sending far too fast
 * would otherwise fill the log, and what an operator needs is that it happened and for how long.
 */
enum class ThrottleOutcome {
    /// Accepted, and the previous command of this kind was accepted too, or there was none.
    Accepted,
    /// Accepted after one or more refusals of this kind. The run of refusals has ended.
    AcceptedAfterRefusals,
    /// Refused, and the previous command of this kind was accepted, or there was none. A run of
    /// refusals has begun.
    FirstRefusal,
    /// Refused, and the previous command of this kind was refused too.
    FurtherRefusal
};

/**
 * @brief Whether the command may be passed on.
 * @param[in] outcome What the throttle decided.
 * @return True for either kind of acceptance.
 */
[[nodiscard]] inline bool is_accepted(ThrottleOutcome outcome) {
    return outcome == ThrottleOutcome::Accepted || outcome == ThrottleOutcome::AcceptedAfterRefusals;
}

} // namespaces
