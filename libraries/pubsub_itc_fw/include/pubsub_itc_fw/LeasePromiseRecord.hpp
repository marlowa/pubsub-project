#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <LeaderStatement.hpp>

namespace fix_common {

/**
 * @brief What an instance keeps on disk about its vote: the promise it has made, and the newest
 *        statement it holds about which instance may not lead.
 *
 * The promise is only meaningful until the machine reboots, because its expiry is a time on the steady
 * clock. The statement does not depend on the clock and is kept across a reboot. See LeasePromiseStore.
 */
struct LeasePromiseRecord {
    int64_t promised_to{0};                        ///< the instance the vote is promised to, or zero for none
    std::chrono::steady_clock::time_point until{}; ///< when the promise runs out
    LeaderStatement statement{};                   ///< the newest statement held; its leader_id is zero if none
};

} // namespaces
