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
// Add new gateways by taking the next free number. Reusing a value would misread any WAL
// still on disk from an older build, so it is worth avoiding -- but it is not the hard
// constraint an earlier version of this comment claimed: the project is pre-1.0 and makes
// no compatibility promise across releases, so a WAL from an older build is discarded
// rather than replayed.

inline constexpr int16_t fix_order_gateway = 1;    ///< The ASCII FIX gateway.
inline constexpr int16_t binary_order_gateway = 2; ///< The binary (DSL PDU) gateway.

// origin_gateway_id is optional, and this is the value a reader uses when it is absent.
//
// Not for backwards compatibility, despite appearances: the FIX gateway never constructs a
// WalRecord at all. It sends a bare NewOrderSingle and the sequencer wraps it, so on that
// path there is nothing upstream that could stamp an origin. The default covers that gap.
// Removing it needs the sequencer to attribute origin from the connection a PDU arrived on
// -- see docs/design/gateway_ha.md, step 2.
inline constexpr int16_t default_when_absent = fix_order_gateway;

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
