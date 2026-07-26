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
 * @brief Identity of one order in the book: which gateway, which session, which ClOrdID.
 *
 * A fixed-size struct, so a lookup allocates nothing.
 *
 * All three parts are needed. ClOrdID alone is only unique within a client session; a
 * session id is only unique within one gateway, because each gateway numbers its own
 * client connections from its own counter. With more than one gateway feeding the book --
 * the ASCII FIX one and the binary one -- the FIX gateway's connection 5 and the binary
 * gateway's connection 5 are unrelated sessions. Were the gateway id left out, two clients
 * that happened to pick the same ClOrdID would share a key: the second order would be
 * rejected as a duplicate, or a cancel from one session would retire the other's order.
 */
struct OrderKey {
    int32_t session_id{};
    int16_t gateway_id{gateway_ids::default_when_absent};
    uint8_t cl_ord_id_len{};
    std::array<char, fix_order_limits::max_cl_ord_id_length> cl_ord_id{};

    /**
     * @brief Builds a key, truncating an over-long ClOrdID to the shared maximum.
     * @param[in] session The originating session's connection id within its gateway.
     * @param[in] gateway Which gateway that session belongs to.
     * @param[in] id      The ClOrdID.
     *
     * Truncation is safe here only because the gateways validate ClOrdID length at
     * ingress against the same limit, rejecting anything longer with an
     * ExecutionReport. Were that check removed, two long ClOrdIDs sharing a prefix
     * would silently become one key.
     */
    static OrderKey make(int32_t session, int16_t gateway, std::string_view id) {
        OrderKey key;
        key.session_id = session;
        key.gateway_id = gateway;
        key.cl_ord_id_len = static_cast<uint8_t>(std::min(id.size(), fix_order_limits::max_cl_ord_id_length));
        std::memcpy(key.cl_ord_id.data(), id.data(), key.cl_ord_id_len);
        return key;
    }

    bool operator==(const OrderKey& other) const {
        return session_id == other.session_id && gateway_id == other.gateway_id && cl_ord_id_len == other.cl_ord_id_len &&
               std::memcmp(cl_ord_id.data(), other.cl_ord_id.data(), cl_ord_id_len) == 0;
    }
};

/** @brief FNV-1a over the session id, then the gateway id, then the ClOrdID bytes. */
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
        for (uint8_t index = 0; index < key.cl_ord_id_len; ++index) {
            hash ^= static_cast<uint8_t>(key.cl_ord_id[index]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

} // namespaces
