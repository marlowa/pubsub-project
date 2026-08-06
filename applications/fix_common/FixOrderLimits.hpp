#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace fix_order_limits {

// Maximum ClOrdID length in bytes. This is a HARD compile-time limit, not a tunable:
// it sizes the fixed-size POD key of the matching engine's flat order-book map and the
// order gateways' open-order pool entry, so both are plain-old-data with no per-lookup
// heap allocation. Because it bounds those keys, every ClOrdID must be measured against
// it at the point it enters the system -- the gateway it arrives through -- and a
// NewOrderSingle or OrderCancelRequest whose ClOrdID (or OrigClOrdID) is longer is
// rejected with an ExecutionReport (Rejected). Keeping the gateway ingress check, the
// open-order pool, and the matching-engine book key on this one value is what prevents
// the silent-truncation key collisions a mismatch would otherwise cause.
inline constexpr size_t max_cl_ord_id_length = 64;

// Maximum comp id length in bytes, and a HARD limit for the same reason as the one above:
// it sizes SessionIdentity, the fixed-size POD key the matching engine's book and the
// sequencer's routing entry are both keyed on.
//
// Unlike a ClOrdID, this one is not enforced at the gateway, because it cannot be exceeded
// by anything that gets that far: a comp id has to be provisioned in the database to
// authenticate, and `pubsub_comp_id.comp_id` is `varchar(64)`. This value is that column's
// width, so the two must be changed together -- widening the column alone would start
// truncating identities, and two comp ids sharing a 64-character prefix would silently
// become one session.
inline constexpr size_t max_comp_id_length = 64;

} // namespaces
