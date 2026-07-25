#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief A tag struct for the ServiceID class.
 *
 * Makes ServiceID a distinct type from any other WrappedInteger id.
 */
struct ServiceIDTag {};

/**
 * @brief A strongly typed id for a registered service.
 *
 * The ServiceRegistry assigns a stable ServiceID to each service as it is
 * registered. Connect requests carry this integer id rather than the service
 * name, so no std::string is copied through the reactor's control-command queue
 * on the per-message path. A default-constructed ServiceID (value 0) is the
 * invalid sentinel returned when a name does not resolve.
 */
using ServiceID = WrappedInteger<ServiceIDTag, int>;

} // namespaces
