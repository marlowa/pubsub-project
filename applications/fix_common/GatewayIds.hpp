#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace gateway_ids {

// Identifies which gateway an order entered through. It rides on the WalRecord envelope
// as origin_gateway_id and is what lets the sequencer send an ExecutionReport back to the
// right one.
//
// This is needed because a client session is identified by its connection id, and that is
// only unique within a single gateway -- each numbers its own client connections from its
// own counter, so the ASCII FIX gateway and the binary gateway will both hand out low
// integers for unrelated sessions. The pair (gateway id, session connection id) is what
// identifies a session across the venue.
//
// Values are part of the on-disk WAL format once written, so they must never be reused for
// a different gateway. Add new gateways by taking the next free number.

inline constexpr int16_t order_gateway = 1;  ///< The ASCII FIX gateway.
inline constexpr int16_t binary_gateway = 2; ///< The binary (DSL PDU) gateway.

// A WalRecord written before origin_gateway_id existed has no gateway id, and every such
// record came from the only gateway there was at the time. Readers treat an absent field
// as this value so old WALs replay and route unchanged.
inline constexpr int16_t default_when_absent = order_gateway;

// Which *instance* of a gateway an order entered through, carried on the envelope as
// gateway_instance_id. The ids above name a protocol, not a process, and cannot be made to
// mean anything else -- they are already written into every WAL record on disk. Running two
// instances of the same protocol therefore needs a second axis, and this is it.
//
// The triple (origin_gateway_id, gateway_instance_id, gateway_session_conn_id) identifies a
// client session venue-wide: the protocol, the process serving it, and the connection within
// that process.
//
// Instances are numbered from one, per protocol. Instance 1 of the FIX gateway and instance 1
// of the binary gateway are different things and do not collide, because origin_gateway_id
// already separates them.
inline constexpr int16_t first_instance = 1;

// Every record written before gateway_instance_id existed came from the only instance running
// at the time. Readers treat an absent field as this value, so those records decode and route
// unchanged -- the same compatibility promise origin_gateway_id makes above.
inline constexpr int16_t default_instance_when_absent = first_instance;

} // namespaces
