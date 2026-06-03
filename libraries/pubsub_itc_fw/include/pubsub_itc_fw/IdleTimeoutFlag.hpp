#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief Typed flag controlling whether a connection or listener observes
 *        the inactivity timeout.
 *
 * Passed to InboundConnectionManager::register_inbound_listener(),
 * Reactor::register_inbound_listener(), and the InboundConnection constructor
 * to make call sites self-documenting.
 *
 * UseIdleTimeout is the normal setting: the reactor disconnects connections
 * that produce no traffic within the configured inactivity window.
 * BypassIdleTimeout is for long-lived framework-internal links that do not
 * exchange heartbeats and must never be torn down by the timeout.
 */
class IdleTimeoutFlag {
  public:
    enum IdleTimeoutFlagTag { UseIdleTimeout = 0, BypassIdleTimeout = 1 };

    explicit IdleTimeoutFlag(IdleTimeoutFlagTag value) : value_{value} {}

    bool isEqual(const IdleTimeoutFlag& rhs) const {
        return value_ == rhs.value_;
    }

    bool isEqual(const IdleTimeoutFlagTag& rhs) const {
        return value_ == rhs;
    }

    IdleTimeoutFlagTag value() const {
        return value_;
    }

  private:
    IdleTimeoutFlagTag value_;
};

inline bool operator==(const IdleTimeoutFlag& lhs, const IdleTimeoutFlag& rhs) {
    return lhs.isEqual(rhs);
}

inline bool operator==(const IdleTimeoutFlag& lhs, const IdleTimeoutFlag::IdleTimeoutFlagTag& rhs) {
    return lhs.isEqual(rhs);
}

inline bool operator==(const IdleTimeoutFlag::IdleTimeoutFlagTag& lhs, const IdleTimeoutFlag& rhs) {
    return rhs.isEqual(lhs);
}

} // namespace pubsub_itc_fw
