#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Which instances of a pair another component has said may not lead.
 *
 * A pair has two instances, numbered 1 and 2, as LeaderStatement numbers them. Both can be barred at once: in the venue,
 * when the leading sequencer has judged the first matching engine to have stopped working, and then the second, which
 * took its place, as well. Neither may then lead, and trading halts by the limit on automatic recovery
 * (docs/availability/a_matching_engine_that_stops_working.md, section 6).
 */
struct InstancesBarredFromLeading {
    // Instance 1 may not lead.
    bool first{false};
    // Instance 2 may not lead.
    bool second{false};

    /// Whether @p instance_id is barred.
    [[nodiscard]] bool contains(int64_t instance_id) const {
        return (instance_id == 1 && first) || (instance_id == 2 && second);
    }

    /// Whether no instance is barred.
    [[nodiscard]] bool empty() const {
        return !first && !second;
    }

    [[nodiscard]] bool operator==(const InstancesBarredFromLeading& other) const {
        return first == other.first && second == other.second;
    }

    [[nodiscard]] bool operator!=(const InstancesBarredFromLeading& other) const {
        return !(*this == other);
    }
};

} // namespaces
