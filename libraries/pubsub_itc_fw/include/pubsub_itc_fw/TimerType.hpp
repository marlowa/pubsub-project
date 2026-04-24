#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief Specifies whether a timer fires once or repeatedly.
 */
class TimerType {
public:
    /**
     * @brief C-style enumeration of timer types.
     */
    enum TimerTypeTag {
        SingleShot, /**< The timer fires once and is then disabled. */
        Recurring   /**< The timer fires repeatedly at a fixed interval. */
    };

    /**
     * @brief Constructs a TimerType from a tag value.
     * @param[in] tag Timer type tag.
     */
    constexpr explicit TimerType(TimerTypeTag tag) : timer_type_(tag) {}

    /**
     * @brief Returns the underlying tag value.
     */
    [[nodiscard]] TimerTypeTag as_tag() const { return timer_type_; }

    /**
     * @brief Returns a string representation of the timer type.
     */
    [[nodiscard]] std::string as_string() const {
        if (timer_type_ == SingleShot) return "SingleShot";
        if (timer_type_ == Recurring)  return "Recurring";
        return "unknown";
    }

private:
    TimerTypeTag timer_type_{SingleShot};
};

inline bool operator==(const TimerType& lhs, const TimerType& rhs) {
    return lhs.as_tag() == rhs.as_tag();
}

inline bool operator!=(const TimerType& lhs, const TimerType& rhs) {
    return !(lhs == rhs);
}

} // namespace pubsub_itc_fw
