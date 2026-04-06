#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The primary and optional secondary network endpoints for a named service.
 *
 * Used by ServiceRegistry to map logical service names to their network addresses.
 * The secondary endpoint is optional: a port value of zero indicates that no
 * secondary address has been configured for this service.
 */
struct ServiceEndpoints {
    NetworkEndpointConfig primary;    ///< Primary endpoint. Must be configured (port != 0).
    NetworkEndpointConfig secondary;  ///< Secondary (fallback) endpoint. port == 0 means not configured.
};

} // namespace pubsub_itc_fw
