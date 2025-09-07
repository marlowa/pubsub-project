#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
// (None directly here)

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief An enumeration that specifies the type of a timer.
 *
 * This enum class defines whether a timer should fire once (single-shot)
 * or repeatedly (recurring).
 */
enum class TimerType {
    SingleShot, /**< The timer fires once and is then disabled. */
    Recurring   /**< The timer fires repeatedly at a fixed interval. */
};

} // namespace pubsub_itc_fw
