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

namespace matching_engine {

/**
 * @brief Identity of one order in the book: which gateway instance, which session, which ClOrdID.
 *
 * A fixed-size struct, so a lookup allocates nothing.
 *
 * All four parts are needed, and each one is there because leaving it out merges two
 * unrelated orders into one key -- so the second is rejected as a duplicate, or a cancel
 * from one session retires the other's order.
 *
 * ClOrdID alone is only unique within a client session. A session id is only unique within
 * one gateway *process*, because each numbers its own client connections from its own
 * counter. That gives the other two axes, and they are genuinely separate:
 *
 * - **gateway_id** separates the protocols. The FIX gateway's connection 5 and the binary
 *   gateway's connection 5 are unrelated sessions.
 * - **gateway_instance** separates the processes within a protocol. Once a protocol runs as
 *   more than one instance, FIX instance a's connection 5 and FIX instance b's connection 5
 *   are unrelated in exactly the same way, and for exactly the same reason. This axis was
 *   missing until instances existed; a key without it is a live collision the moment a
 *   second instance of any protocol takes orders.
 *
 * The triple (gateway_id, gateway_instance, session_id) is what GatewayIds.hpp calls a
 * venue-wide session identity; this key is that plus the ClOrdID.
 */
struct OrderKey {
    int32_t session_id{};
    int16_t gateway_id{gateway_ids::default_when_absent};
    int16_t gateway_instance{gateway_ids::first_instance};
    uint8_t cl_ord_id_len{};
    std::array<char, fix_order_limits::max_cl_ord_id_length> cl_ord_id{};

    /**
     * @brief Builds a key, truncating an over-long ClOrdID to the shared maximum.
     * @param[in] session  The originating session's connection id within its gateway instance.
     * @param[in] gateway   Which gateway protocol that session belongs to.
     * @param[in] instance  Which instance of that protocol. Deliberately NOT defaulted: this
     *                      axis was forgotten once already, and a default would let the next
     *                      call site forget it silently rather than failing to compile.
     * @param[in] id        The ClOrdID.
     *
     * Truncation is safe here only because the gateways validate ClOrdID length at
     * ingress against the same limit, rejecting anything longer with an
     * ExecutionReport. Were that check removed, two long ClOrdIDs sharing a prefix
     * would silently become one key.
     */
    static OrderKey make(int32_t session, int16_t gateway, int16_t instance, std::string_view id) {
        OrderKey key;
        key.session_id = session;
        key.gateway_id = gateway;
        key.gateway_instance = instance;
        key.cl_ord_id_len = static_cast<uint8_t>(std::min(id.size(), fix_order_limits::max_cl_ord_id_length));
        std::memcpy(key.cl_ord_id.data(), id.data(), key.cl_ord_id_len);
        return key;
    }

    bool operator==(const OrderKey& other) const {
        return session_id == other.session_id && gateway_id == other.gateway_id && gateway_instance == other.gateway_instance &&
               cl_ord_id_len == other.cl_ord_id_len && std::memcmp(cl_ord_id.data(), other.cl_ord_id.data(), cl_ord_id_len) == 0;
    }
};

/** @brief FNV-1a over the session id, the gateway id, the gateway instance, then the ClOrdID bytes. */
struct OrderKeyHash {
    size_t operator()(const OrderKey& key) const {
        size_t hash = 14695981039346656037ULL;
        const auto* session_bytes = reinterpret_cast<const uint8_t*>(&key.session_id);
        for (size_t index = 0; index < sizeof(key.session_id); ++index) {
            hash ^= session_bytes[index];
            hash *= 1099511628211ULL;
        }
        const auto* gateway_bytes = reinterpret_cast<const uint8_t*>(&key.gateway_id);
        for (size_t index = 0; index < sizeof(key.gateway_id); ++index) {
            hash ^= gateway_bytes[index];
            hash *= 1099511628211ULL;
        }
        const auto* instance_bytes = reinterpret_cast<const uint8_t*>(&key.gateway_instance);
        for (size_t index = 0; index < sizeof(key.gateway_instance); ++index) {
            hash ^= instance_bytes[index];
            hash *= 1099511628211ULL;
        }
        for (uint8_t index = 0; index < key.cl_ord_id_len; ++index) {
            hash ^= static_cast<uint8_t>(key.cl_ord_id[index]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

} // namespaces
