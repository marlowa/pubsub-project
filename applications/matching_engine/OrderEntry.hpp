#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string_view>

#include <fix_orders.hpp>

#include "FixOrderLimits.hpp"
#include "SessionIdentity.hpp"

namespace matching_engine {

// ClOrdID length is the single shared limit (fix_order_limits): the gateway validates
// every inbound ClOrdID against it, so this fixed-size book key is never truncated.
// Symbol max ~12; 16 is ample. Quantities and prices are decimal strings; 24 covers any realistic value.
constexpr size_t max_cl_ord_id_length = fix_order_limits::max_cl_ord_id_length;
constexpr size_t max_symbol_length = 16;
constexpr size_t max_qty_length = 24;

// Live order stored in the order book from NOS acceptance until cancel.
// All string fields stored as fixed-size char arrays -- no heap allocation.
struct OrderEntry {
    int64_t order_id_num{}; // counter value; formatted to "ME-ORD-N" on demand
    // Whose order this is: the comp id and the protocol, which together are the key
    // this entry is filed under and the address-free way to reach the member again.
    //
    // It used to be the originating connection id, plus the protocol and instance
    // needed to disambiguate it. That identified a socket rather than a member, which
    // failed in exactly the case this field exists for: a cancel-on-failover ER is
    // emitted when a process has died, so the connection it named was already gone,
    // and a member reconnecting elsewhere could neither be sent the report nor cancel
    // what it had left resting. The sequencer now resolves this identity to wherever
    // the session is currently bound. See docs/availability/gateway_ha.md.
    fix_common::SessionIdentity session;
    pubsub_itc_fw_app::Side side{};
    pubsub_itc_fw_app::OrdType ord_type{};
    // Echoed back on every ExecutionReport for this order. The gateway needs it to
    // decide whether cancel-on-disconnect applies: a GoodTillCancel or GoodTillDate
    // order is by definition meant to outlive the session that placed it, so a
    // dropped socket must not retire it. has_time_in_force is false when the client
    // sent no tag 59, in which case no exemption is claimed and Day is implied.
    bool has_time_in_force{false};
    pubsub_itc_fw_app::TimeInForce time_in_force{};
    // When a GoodTillDate order expires, exactly as the member stated it.
    //
    // Echoed back unaltered, and stored here rather than re-read from the order because
    // reports are emitted long after the NewOrderSingle has gone.
    //
    // Worth saying explicitly, where the same about the symbol or the quantity would be
    // noise: **adjusting a GTD expiry is a real venue behaviour**, not a bug. Venues
    // truncate to a maximum lifetime, round to session close, or resolve a date against
    // a trading calendar, and are not misbehaving when they do. This venue does not, so
    // the member's stated instant is the one that governs. That is a choice between
    // defensible alternatives, which is why it is written down and tested rather than
    // left to be inferred from the absence of conversion code.
    bool has_expire_time{false};
    int64_t expire_time{};
    bool has_price{false};
    uint8_t symbol_len{};
    uint8_t order_qty_len{};
    uint8_t price_len{};
    std::array<char, max_symbol_length> symbol{};
    std::array<char, max_qty_length> order_qty{};
    std::array<char, max_qty_length> price{};

    void set_symbol(std::string_view sv) {
        symbol_len = static_cast<uint8_t>(std::min(sv.size(), max_symbol_length));
        std::memcpy(symbol.data(), sv.data(), symbol_len);
    }
    void set_order_qty(std::string_view sv) {
        order_qty_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
        std::memcpy(order_qty.data(), sv.data(), order_qty_len);
    }
    void set_price(std::string_view sv) {
        price_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
        std::memcpy(price.data(), sv.data(), price_len);
    }

    [[nodiscard]] std::string_view get_symbol() const {
        return {symbol.data(), symbol_len};
    }
    [[nodiscard]] std::string_view get_order_qty() const {
        return {order_qty.data(), order_qty_len};
    }
    [[nodiscard]] std::string_view get_price() const {
        return {price.data(), price_len};
    }
};

} // namespaces
