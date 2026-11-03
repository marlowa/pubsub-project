#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string_view>

namespace fix_common {

/**
 * @brief Whether a gateway must refuse a member's command instead of sending it to the sequencers, and why.
 *
 * Both gateways keep a copy of every command they send until it is answered (`pubsub_itc_fw::UnansweredCommands`). Keeping the
 * copy can fail in two ways, and each is answered to the member with a rejection rather than by sending the command.
 */
enum class GatewayRefusal {
    /// The command was sent, or the caller has nothing to refuse.
    None,
    /// The store of unanswered commands is full: so many commands wait for an answer that the venue is not answering
    /// them. The member is told the venue is busy.
    StoreFull,
    /// A command with the same comp id and ClOrdID is already waiting for its answer. Sending this one would make the
    /// two indistinguishable when their answers arrive, because a gateway matches an answer to its copy by comp id and
    /// ClOrdID (docs/availability/one_check_against_sequencing_twice.md, section 4.3).
    IdentifierHeldUnanswered
};

/// The text both gateways give a member whose command reuses a ClOrdID, whether the gateway or the sequencer refused it.
inline constexpr std::string_view identifier_already_used_text = "ClOrdID already used today";

} // namespaces
