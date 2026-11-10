#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace fix_common {

/**
 * @brief Why a gateway refused a member's request itself, before the request reached the matching engine.
 *
 * The numbers are the ones the FIX gateway gives in SessionRejectReason (373) on the session-level Reject (35=3) it
 * sends, and the binary gateway in the reason of its RequestRefused message, so that the two gateways give the same
 * answer to the same refusal (docs/venue/gateway_refusals.md). 1, 5 and 99 are the FIX standard's values. The standard
 * defines none for a throttle or a busy venue, and leaves values from 100 to be agreed between the parties, so 100 and
 * 101 are the venue's own.
 */
enum class RefusalReason : int32_t {
    RequiredFieldMissing = 1,    ///< a field the venue requires is absent; the refusal names the field
    ValueIncorrect = 5,          ///< a field holds a value the venue does not accept, or is too long; the refusal names the field
    Other = 99,                  ///< any other reason, such as a ClOrdID used before; the text says which
    ThrottleLimitExceeded = 100, ///< the session is over its limit for this kind of request; the text names the limit
    VenueUnavailable = 101       ///< no sequencer is connected, the venue is not taking commands, or too many commands wait to be answered
};

} // namespaces
