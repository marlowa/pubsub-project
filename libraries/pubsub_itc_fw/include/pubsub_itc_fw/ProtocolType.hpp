#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/**
 * @brief Categorises the byte-stream handling logic for an InboundConnection.
 *
 * @ingroup reactor_subsystem
 *
 * This type determines the processing pipeline the Reactor employs for incoming
 * socket data on a given listener.
 *
 * Modes of operation:
 *   FrameworkPdu — the Reactor expects structured data containing framework
 *   headers. It uses PduParser and ExpandableSlabAllocator to extract discrete
 *   messages before delivery to the application via the Vyukov queue.
 *
 *   RawBytes — the Reactor treats the connection as an opaque pipe. Bytes are
 *   read directly into a MirroredBuffer to provide the application with a
 *   contiguous view of the stream. No framework-level parsing is performed.
 */
class ProtocolType {
  public:
    enum ProtocolTypeTag { FrameworkPdu = 0, RawBytes = 1 };

    /**
     * @brief Initialises the protocol type with a specific handling mode.
     */
    constexpr explicit ProtocolType(ProtocolTypeTag value) : value_{value} {}

    /**
     * @brief Checks equality with another ProtocolType instance.
     */
    [[nodiscard]] bool is_equal(const ProtocolType& rhs) const {
        return value_ == rhs.value_;
    }

    /**
     * @brief Checks equality with a ProtocolTypeTag.
     */
    [[nodiscard]] bool is_equal(ProtocolTypeTag rhs) const {
        return value_ == rhs;
    }

    /**
     * @brief Returns the underlying tag value.
     */
    [[nodiscard]] ProtocolTypeTag value() const {
        return value_;
    }

  private:
    ProtocolTypeTag value_;
};

inline bool operator==(const ProtocolType& lhs, const ProtocolType& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const ProtocolType& lhs, ProtocolType::ProtocolTypeTag rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(ProtocolType::ProtocolTypeTag lhs, const ProtocolType& rhs) {
    return rhs.is_equal(lhs);
}

} // namespace pubsub_itc_fw
