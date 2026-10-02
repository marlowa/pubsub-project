#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string_view>

#include <tsl/robin_map.h>

#include "FixOrderLimits.hpp"

namespace open_orders {

// Hard structural ceilings for the symbol/qty open-order string fields (compile-time
// char-array sizes in OpenOrderEntry). Runtime-configurable [fix_limits] limits must be
// <= these and are enforced at startup; over-length symbol/qty get a FIX BusinessReject.
// ClOrdID instead uses the single shared fix_order_limits::max_cl_ord_id_length -- validated
// at ingress with an ExecutionReport rejection -- so the gateway pool and the matching-engine
// book key stay on one value. See docs/venue/fix_order_gateway.md.
//
// Why fixed char arrays rather than std::string or a general pool:
//   std::string causes one heap allocation per field per order, measured at
//   1.21% of gateway CPU at 122K orders/second. A general variable-size pool
//   with individual frees requires implementing boundary-tag coalescing --
//   complexity comparable to ExpandablePoolAllocator, which itself was very
//   hard to write correctly. Fixed char arrays with ExpandablePoolAllocator
//   and tsl::robin_map eliminate all per-order heap allocation with a
//   manageable implementation cost.
inline constexpr size_t max_supported_symbol_length = 64;
inline constexpr size_t max_supported_order_qty_length = 32;
// The matching engine's identifiers are "ME-ORD-" followed by a 64-bit counter, at most 26
// characters, so 32 holds every one with room to spare.
inline constexpr size_t max_supported_order_id_length = 32;

/**
 * @brief Pool-allocated storage for a single open order's string fields.
 *
 * All string data is held inline in fixed char arrays -- no heap allocation per order. An
 * OpenOrderEntry is allocated from a gateway's open-order pool when the matching engine
 * sends a non-terminal ExecutionReport, and returned to the pool when a terminal ER
 * arrives or when the cancel drain sends the OrderCancelRequest on disconnect.
 *
 * The map below holds std::string_view keys that point directly into cl_ord_id[] here, so
 * the pool entry must remain alive for the lifetime of the map entry.
 *
 * This lives in fix_common rather than in either gateway because cancelling a dead
 * session's resting orders is a property of being a gateway, not of speaking FIX: the
 * binary gateway has exactly the same obligation to the book.
 */
struct OpenOrderEntry {
    char cl_ord_id[fix_order_limits::max_cl_ord_id_length + 1]{};
    uint8_t cl_ord_id_len{0};
    char symbol[max_supported_symbol_length + 1]{};
    uint8_t symbol_len{0};
    char order_qty[max_supported_order_qty_length + 1]{};
    uint8_t order_qty_len{0};
    char side{0};
    // TimeInForce as its raw FIX tag-59 character, taken from the ExecutionReport that put
    // this order on the book. Stored as a char rather than the generated enum so this
    // struct stays free of any protocol header: it is shared with the binary gateway,
    // which never sees FIX text. Zero means the order carried no TimeInForce, which
    // implies Day and claims no exemption.
    //
    // It is here for cancel-on-disconnect: see is_persistent_time_in_force().
    char time_in_force{0};
    // The OrderID the matching engine assigned, from the ExecutionReport that put this order on
    // the book. A gateway that refuses a request to cancel the order puts it on its
    // OrderCancelReject, which FIX requires to carry the order's OrderID. Empty when the engine's
    // identifier did not fit, in which case the reply says "NONE".
    char order_id[max_supported_order_id_length + 1]{};
    uint8_t order_id_len{0};
};

/**
 * @brief Records @p order_id in @p entry, or records none if it is too long to hold.
 *
 * Too long cannot happen with the matching engine's identifiers (see
 * max_supported_order_id_length); refusing it rather than truncating it means a reply never
 * names an order by a shortened identifier that might be another order's.
 */
inline void set_order_id(OpenOrderEntry& entry, std::string_view order_id) {
    if (order_id.size() > max_supported_order_id_length) {
        entry.order_id_len = 0;
        entry.order_id[0] = '\0';
        return;
    }
    std::memcpy(entry.order_id, order_id.data(), order_id.size());
    entry.order_id[order_id.size()] = '\0';
    entry.order_id_len = static_cast<uint8_t>(order_id.size());
}

// TimeInForce values that make an order outlive the session that placed it: GoodTillCancel
// ('1') and GoodTillDate ('6'). Cancel-on-disconnect must leave these resting -- a member
// that asked for good-till-cancel did not ask for "until my socket drops", and cancelling
// them on a dropped connection silently overrides an explicit instruction.
//
// The characters are the FIX tag-59 wire values, matching pubsub_itc_fw_app::TimeInForce
// (GoodTillCancel = 49 = '1', GoodTillDate = 54 = '6'). They are spelled as characters
// rather than referencing the enum so this header stays protocol-free; the gateway unit
// tests assert the two agree, so a data-dictionary change cannot silently desynchronise
// them.
inline constexpr char time_in_force_good_till_cancel = '1';
inline constexpr char time_in_force_good_till_date = '6';

/** @brief Whether this TimeInForce means the order should survive a lost session. */
inline constexpr bool is_persistent_time_in_force(char time_in_force) {
    return time_in_force == time_in_force_good_till_cancel || time_in_force == time_in_force_good_till_date;
}

/**
 * @brief A session's open orders, keyed by ClOrdID.
 *
 * Key: a std::string_view over the corresponding OpenOrderEntry::cl_ord_id[], which is
 * stable for the pool entry's lifetime. Looking up by string_view compares contents, so a
 * lookup needs no std::string to be constructed.
 */
using OpenOrderMap = tsl::robin_map<std::string_view, OpenOrderEntry*, std::hash<std::string_view>, std::equal_to<std::string_view>>;

} // namespaces
