#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Tag struct for ConnectionID.
 *
 * Serves as a unique tag to make ConnectionID distinct from all other
 * WrappedInteger instantiations. Has no members.
 */
struct ConnectionIDTag {};

/**
 * @brief A strongly typed identifier for a TCP connection managed by the Reactor.
 *
 * ConnectionIDs are assigned by the Reactor when a connection is established,
 * either inbound (via TcpAcceptor) or outbound (via TcpConnector). They are
 * monotonically increasing integers starting from 1. A value of zero indicates
 * an invalid or unestablished connection (see is_valid()).
 *
 * ConnectionIDs are passed to ApplicationThreads via ConnectionEstablished
 * EventMessages and are used in SendPdu ReactorControlCommands to identify
 * the target socket.
 */
using ConnectionID = WrappedInteger<ConnectionIDTag, int>;

} // namespace pubsub_itc_fw
