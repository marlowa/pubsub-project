#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <string_view>

#include "FixOrderLimits.hpp"
#include "GatewayIds.hpp"
#include "SessionIdentity.hpp"

namespace matching_engine {

/**
 * @brief Identity of one order in the book: which session, and which ClOrdID within it.
 *
 * A fixed-size struct, so a lookup allocates nothing.
 *
 * ClOrdID alone is only unique within a client session, so the session has to be part of
 * the key -- otherwise two members using the same ClOrdID collide, and a cancel from one
 * retires the other's order.
 *
 * **The session is named by its identity, not by the connection it arrived on**, and that
 * is what makes an order outlive a reconnect. The key used to be
 * (connection id, protocol, instance, ClOrdID), which named a socket on a process: when a
 * member's connection dropped, its resting orders stayed in the book under a key nothing
 * could reproduce, so the member could see them on reconnect but not cancel them -- and
 * after a gateway failover it could not even do that, because the instance in the key was
 * a process that no longer existed. Keying on the identity means a member that comes back
 * -- to the same instance or to its backup -- builds the same key and can manage what it
 * left resting. See docs/design/gateway_ha.md.
 *
 * SessionIdentity carries the protocol, and deliberately not the instance: an instance
 * failover moves a session between instances of one protocol, so including it would defeat
 * the purpose, while a FIX and a binary session sharing a comp id are genuinely separate
 * and must not share a book.
 */
struct OrderKey {
    fix_common::SessionIdentity session;
    uint8_t cl_ord_id_len{};
    std::array<char, fix_order_limits::max_cl_ord_id_length> cl_ord_id{};

    /**
     * @brief Builds a key, truncating an over-long ClOrdID to the shared maximum.
     * @param[in] session The identity of the session that placed the order.
     * @param[in] id      The ClOrdID.
     *
     * Truncation is safe here only because the gateways validate ClOrdID length at
     * ingress against the same limit, rejecting anything longer with an
     * ExecutionReport. Were that check removed, two long ClOrdIDs sharing a prefix
     * would silently become one key.
     */
    static OrderKey make(const fix_common::SessionIdentity& session, std::string_view id) {
        OrderKey key;
        key.session = session;
        key.cl_ord_id_len = static_cast<uint8_t>(std::min(id.size(), fix_order_limits::max_cl_ord_id_length));
        std::memcpy(key.cl_ord_id.data(), id.data(), key.cl_ord_id_len);
        return key;
    }

    bool operator==(const OrderKey& other) const {
        return session == other.session && cl_ord_id_len == other.cl_ord_id_len && std::memcmp(cl_ord_id.data(), other.cl_ord_id.data(), cl_ord_id_len) == 0;
    }
};

/** @brief FNV-1a over the session identity, then the ClOrdID bytes. */
struct OrderKeyHash {
    size_t operator()(const OrderKey& key) const {
        size_t hash = fix_common::SessionIdentityHash{}(key.session);
        for (uint8_t index = 0; index < key.cl_ord_id_len; ++index) {
            hash ^= static_cast<uint8_t>(key.cl_ord_id[index]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

} // namespaces
