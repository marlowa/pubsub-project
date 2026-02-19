#pragma once

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

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
