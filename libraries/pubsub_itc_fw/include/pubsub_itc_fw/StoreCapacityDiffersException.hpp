#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Thrown by MappedSlotStore::open() when the file is a valid store, written with the record size, record format
 *        and version asked for, but holding a different number of records from the number asked for.
 *
 * It is told apart from the other refusals because the store is intact: its header and its records can be read, and
 * the file is left exactly as it was. A store whose header is damaged, or which was written by a different build, is
 * refused with PubSubItcException, of which this is a kind, so a caller that does not care which it was can catch that.
 *
 * A caller that keeps something in the store which must be the same from one start to the next, such as the matching
 * engine's open orders, uses this to refuse to start rather than rebuild with the capacity it was given. See
 * docs/availability/region_capacity_changed_during_a_day.md.
 */
class StoreCapacityDiffersException : public PubSubItcException {
  public:
    /**
     * @param[in] message          The whole explanation, naming the file and both capacities.
     * @param[in] capacity_held    How many records the store was created to hold.
     * @param[in] capacity_wanted  How many records the caller asked for.
     */
    StoreCapacityDiffersException(const std::string& message, uint32_t capacity_held, uint32_t capacity_wanted)
        : PubSubItcException(message), capacity_held_(capacity_held), capacity_wanted_(capacity_wanted) {}

    /// How many records the store was created to hold.
    [[nodiscard]] uint32_t capacity_held() const {
        return capacity_held_;
    }

    /// How many records the caller asked for.
    [[nodiscard]] uint32_t capacity_wanted() const {
        return capacity_wanted_;
    }

  private:
    uint32_t capacity_held_;
    uint32_t capacity_wanted_;
};

} // namespaces
