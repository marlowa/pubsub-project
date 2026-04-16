#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Configuration data for a single inbound TCP listener.
 *
 * This struct holds the static configuration fields that describe how an
 * inbound listener should be set up. It is intentionally free of runtime
 * state and suitable for use in configuration files and programmatic setup.
 *
 * It is owned by InboundListener alongside the runtime state (TcpAcceptor,
 * current ConnectionID) that is managed exclusively by the Reactor.
 *
 * @see InboundListener
 */
struct InboundListenerConfiguration {
    /**
     * @brief The address and port this listener binds to.
     */
    NetworkEndpointConfiguration address;

    /**
     * @brief The ApplicationThread that receives all events and data from
     *        connections accepted on this listener.
     */
    ThreadID target_thread_id;

    /**
     * @brief Determines which protocol handler is constructed for accepted connections.
     *
     * FrameworkPdu — constructs PduProtocolHandler (default).
     * RawBytes     — constructs RawBytesProtocolHandler.
     */
    ProtocolType protocol_type{ProtocolType::FrameworkPdu};

    /**
     * @brief Minimum capacity of the MirroredBuffer in bytes for RawBytes listeners.
     *
     * Ignored for FrameworkPdu listeners. For RawBytes listeners this is passed
     * directly to the RawBytesProtocolHandler constructor and rounded up to the
     * nearest page size internally. Must be greater than zero when
     * protocol_type == RawBytes.
     */
    int64_t raw_buffer_capacity{0};
};

} // namespace pubsub_itc_fw
