#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/TlsClientConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The primary and optional secondary network endpoints for a named service,
 *        plus optional TLS configuration.
 *
 * Used by ServiceRegistry to map logical service names to their network addresses.
 * The secondary endpoint is optional: a port value of zero indicates that no
 * secondary address has been configured for this service.
 * When tls has a value the framework establishes TLS on every outbound connection
 * to this service; without it the connection is plain TCP.
 */
struct ServiceEndpoints {
    NetworkEndpointConfiguration primary;   ///< Primary endpoint. Must be configured (port != 0).
    NetworkEndpointConfiguration secondary; ///< Secondary (fallback) endpoint. port == 0 means not configured.
    std::optional<TlsClientConfiguration> tls; ///< Present when the service requires TLS.
};

} // namespaces
