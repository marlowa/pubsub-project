#pragma once

// Project headers
#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A tag struct for the TimerID class.
 *
 * This struct serves as a unique tag to make the TimerID distinct from
 * any other ID class. It has no members and serves no purpose other than
 * as a template parameter for the generic `WrappedInteger` class.
 */
struct TimerIDTag {};

/**
 * @brief A strongly typed ID for a timer.
 *
 * This class is an alias of the generic `WrappedInteger` template, instantiated with
 * the `TimerIDTag`. This ensures that a `TimerID` is a distinct type from
 * other IDs in the framework.
 */
using TimerID = WrappedInteger<TimerIDTag, int>;

} // namespace pubsub_itc_fw
