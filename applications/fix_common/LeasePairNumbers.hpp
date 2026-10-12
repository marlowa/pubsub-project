#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief The number the library's lease messages carry for one of the venue's pairs.
 *
 * The lease messages belong to the library, which knows nothing of the venue's pairs, so they name a
 * pair by a plain number (pair_id). The venue's pairs are the values of ComponentGroup, and the number
 * for each is its value: sequencer 1, matching engine 2, matching engine publisher 3, arbiter 4.
 */
inline int32_t pair_id_of(pubsub_itc_fw_app::ComponentGroup group) {
    return static_cast<int32_t>(group);
}

/**
 * @brief The venue's pair whose number a lease message carries.
 *
 * A number that is not one of the venue's pairs gives a ComponentGroup value with no name, which the
 * caller refuses as it refuses any group it does not take part in.
 */
inline pubsub_itc_fw_app::ComponentGroup group_of(int32_t pair_id) {
    return static_cast<pubsub_itc_fw_app::ComponentGroup>(pair_id);
}

} // namespaces
