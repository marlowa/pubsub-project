#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Identifies a byte position within a segmented WAL.
 *
 * Used as the anchor between WalReader (replay) and WalWriter (write
 * resumption), and stored in application snapshots so that WAL replay
 * can begin at the snapshot anchor rather than from segment 0.
 */
struct WalPosition {
    uint64_t segment{0}; ///< Zero-based segment file index.
    uint64_t offset{0};  ///< Byte offset within that segment.
};

} // namespaces
