#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace fix_order_limits {

// Maximum ClOrdID length in bytes. This is a HARD compile-time limit, not a tunable:
// it sizes the fixed-size POD key of the matching engine's flat order-book map, so that
// key is plain-old-data with no per-lookup heap allocation. Because it bounds that key,
// every ClOrdID must be measured against it at the point it enters the system -- the
// gateway it arrives through -- and a NewOrderSingle whose ClOrdID is longer is rejected
// with an ExecutionReport (Rejected), and an OrderCancelRequest whose ClOrdID or
// OrigClOrdID is longer is refused. Keeping the gateway ingress check and the
// matching-engine book key on this one value is what prevents the silent-truncation key
// collisions a mismatch would otherwise cause.
inline constexpr size_t max_cl_ord_id_length = 64;

// The longest Symbol, OrderQty and Price the venue holds, in bytes. Each is a hard limit for the
// same reason as the ClOrdID's: it sizes a fixed-size field of the matching engine's OrderEntry.
// A gateway's configured max_symbol_length and max_order_qty_length may not exceed these, and the
// matching engine refuses a new order with a longer field rather than storing it cut short.
//
// 35 is the longest Financial Instrument Short Name (FISN, ISO 18774): an issuer name of up to 15
// characters, a slash, and a description of up to 19. FIX sets no maximum for Symbol (55), so the
// venue takes the longest standard short name for an instrument as its own. A quantity or price is
// a decimal number written as text, and 24 characters hold any value the venue trades.
inline constexpr size_t max_symbol_length = 35;
inline constexpr size_t max_order_qty_length = 24;
inline constexpr size_t max_price_length = 24;

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
