#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <leader_follower.hpp>

namespace fix_common {

/// One void: the position of its VoidCommands record in the WAL, and the commands it voids.
struct VoidedRange {
    int64_t void_seq_no{0};  ///< where the VoidCommands record is in the WAL
    int64_t first_seq_no{0}; ///< the first command it voids
    int64_t last_seq_no{0};  ///< the last command it voids, included
    bool new_orders_only{false};
    pubsub_itc_fw_app::VoidReason reason{pubsub_itc_fw_app::VoidReason::EngineDiedApplying};
};

/// Whether a command asked about is a new order, which every void voids, or a cancel, which only a void without
/// new_orders_only does.
enum class VoidedKind { new_order, cancel };

/**
 * @brief The voids in one trading day's WAL: which commands are answered with a rejection instead of being applied.
 *
 * A VoidCommands record in the WAL says that commands before it are not applied. A matching engine reaches those
 * commands before it reaches the record, so the leading sequencer keeps this list from its WAL and sends it to every
 * engine before each catch-up and whenever it writes a void, and the engine keeps the list it is sent. Programs that
 * apply the WAL themselves read every VoidCommands record into one of these first. See
 * docs/availability/voided_commands.md, section 3.
 *
 * A day has few voids, so the list is a vector, kept in the order of the voids' positions, and a lookup reads it from
 * the start. Looking up a command allocates nothing; adding a void may.
 *
 * Not thread-safe. Each holder keeps its own, on one thread.
 */
class VoidedCommandList {
  public:
    /**
     * @brief Adds a void, unless the list holds one at the same position already.
     * @return true when it was added.
     */
    bool add(const VoidedRange& voided) {
        if (at(voided.void_seq_no) != nullptr) {
            return false;
        }
        const auto later = std::find_if(voids_.begin(), voids_.end(), [&](const VoidedRange& held) { return held.void_seq_no > voided.void_seq_no; });
        voids_.insert(later, voided);
        return true;
    }

    /**
     * @brief The void that voids the member's command at a position, or null if none does.
     * @param[in] sequence_number The command's position in the WAL.
     * @param[in] kind            Whether it is a new order or a cancel.
     */
    [[nodiscard]] const VoidedRange* voiding(int64_t sequence_number, VoidedKind kind) const {
        for (const VoidedRange& held : voids_) {
            if (sequence_number >= held.first_seq_no && sequence_number <= held.last_seq_no && (kind == VoidedKind::new_order || !held.new_orders_only)) {
                return &held;
            }
        }
        return nullptr;
    }

    /// The void whose VoidCommands record is at this position, or null.
    [[nodiscard]] const VoidedRange* at(int64_t void_seq_no) const {
        for (const VoidedRange& held : voids_) {
            if (held.void_seq_no == void_seq_no) {
                return &held;
            }
        }
        return nullptr;
    }

    /// Removes every void whose VoidCommands record is after @p seq_no: a sequencer's WAL has discarded those records.
    void discard_after(int64_t seq_no) {
        voids_.erase(std::remove_if(voids_.begin(), voids_.end(), [&](const VoidedRange& held) { return held.void_seq_no > seq_no; }), voids_.end());
    }

    /// Every void, in the order of their positions.
    [[nodiscard]] const std::vector<VoidedRange>& voids() const {
        return voids_;
    }

    [[nodiscard]] bool empty() const {
        return voids_.empty();
    }

    [[nodiscard]] size_t size() const {
        return voids_.size();
    }

  private:
    std::vector<VoidedRange> voids_;
};

} // namespaces
