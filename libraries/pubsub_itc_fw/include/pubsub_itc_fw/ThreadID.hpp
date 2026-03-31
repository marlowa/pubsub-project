#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

static constexpr int system_thread_id_value = 0;

/**
 * @brief A tag struct for the ThreadID class.
 *
 * This struct serves as a unique tag to make the ThreadID distinct from
 * any other ID class. It has no members and serves no purpose other than
 * as a template parameter for the generic `WrappedInteger` class.
 */
struct ThreadIDTag {};

/**
 * @brief A strongly typed ID for a timer.
 *
 * This class is an alias of the generic `WrappedInteger` template, instantiated with
 * the `ThreadIDTag`. This ensures that a `ThreadID` is a distinct type from
 * other IDs in the framework.
 */
using ThreadID = WrappedInteger<ThreadIDTag, int>;

} // namespace pubsub_itc_fw
