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

namespace fix_common {

/**
 * @brief Which session a venue-internal record belongs to: a comp id and a protocol.
 *
 * This is the identity that outlives a connection, and it exists because the thing that
 * used to stand in for it did not. A session used to be identified by the triple
 * (protocol, instance, connection id), which names a *socket on a process*: it dies when
 * the socket does, it is renumbered on reconnect, and it is not even the same number at
 * the member's backup gateway. So a member that reconnected could not be handed reports
 * for orders it had already placed, and could not cancel them either -- the order was
 * filed under an address that no longer existed. See docs/design/gateway_ha.md.
 *
 * The triple is still how a report is *delivered*. It is simply no longer what a session
 * *is*: identity is this struct, the connection triple is a mutable destination bound to
 * it, and a logon re-binds one to the other.
 *
 * **The protocol is part of the identity; the instance deliberately is not.** An instance
 * failover moves a session between instances of one protocol, so including the instance
 * would defeat the entire purpose. But a FIX session and a binary session are separate
 * sessions even when one member holds both, so they must not share a book or each other's
 * reports -- and with the comp id alone they would.
 *
 * Fixed-size and trivially copyable, like OrderKey and OrderEntry beside it: it is stored
 * per live order and per sequenced order, so a heap allocation here would be one per
 * order on the hot path.
 *
 * Truncation is not reachable rather than merely unlikely. A comp id must be provisioned
 * in the database to authenticate at all, `pubsub_comp_id.comp_id` is `varchar(64)`, and
 * the capacity here is that same 64 -- so anything that can log on fits. The length is
 * still clamped rather than trusted, because a silent overrun would be a far worse way to
 * find out that assumption had changed.
 */
struct SessionIdentity {
    /// Which order-entry protocol the session speaks; see GatewayIds.hpp.
    int16_t protocol{gateway_ids::default_when_absent};
    uint8_t comp_id_len{};
    std::array<char, fix_order_limits::max_comp_id_length> comp_id{};

    static SessionIdentity make(std::string_view comp_id, int16_t protocol) {
        SessionIdentity identity;
        identity.protocol = protocol;
        identity.comp_id_len = static_cast<uint8_t>(std::min(comp_id.size(), fix_order_limits::max_comp_id_length));
        std::memcpy(identity.comp_id.data(), comp_id.data(), identity.comp_id_len);
        return identity;
    }

    /// The comp id as text, valid as long as this struct is.
    [[nodiscard]] std::string_view comp_id_view() const {
        return std::string_view(comp_id.data(), comp_id_len);
    }

    /// True when this identity names nothing -- a record with no originating client session.
    [[nodiscard]] bool empty() const {
        return comp_id_len == 0;
    }

    bool operator==(const SessionIdentity& other) const {
        return protocol == other.protocol && comp_id_len == other.comp_id_len && std::memcmp(comp_id.data(), other.comp_id.data(), comp_id_len) == 0;
    }

    bool operator!=(const SessionIdentity& other) const {
        return !(*this == other);
    }
};

/** @brief FNV-1a over the protocol then the comp id bytes. */
struct SessionIdentityHash {
    size_t operator()(const SessionIdentity& identity) const {
        size_t hash = 14695981039346656037ULL;
        const auto* protocol_bytes = reinterpret_cast<const uint8_t*>(&identity.protocol);
        for (size_t index = 0; index < sizeof(identity.protocol); ++index) {
            hash ^= protocol_bytes[index];
            hash *= 1099511628211ULL;
        }
        for (uint8_t index = 0; index < identity.comp_id_len; ++index) {
            hash ^= static_cast<uint8_t>(identity.comp_id[index]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

/**
 * @brief Where a session's reports go right now: a connection on a gateway instance.
 *
 * Held against a SessionIdentity and replaced wholesale on each logon. The protocol is not
 * repeated here because it is part of the identity this is bound to and cannot change: a
 * session does not migrate between protocols.
 */
struct SessionDestination {
    int16_t instance{gateway_ids::first_instance};
    int32_t conn_id{};
};

} // namespaces
