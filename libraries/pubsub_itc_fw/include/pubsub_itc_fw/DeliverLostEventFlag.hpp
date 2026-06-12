#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/** @ingroup utilities_subsystem */

/**
 * @brief Typed flag controlling whether a ConnectionLost event is delivered
 *        to the ApplicationThread when a connection is torn down.
 *
 * Passed to InboundConnectionManager::teardown_connection() and
 * OutboundConnectionManager::teardown_connection() to make call sites
 * self-documenting.
 *
 * The name deserves explanation. "Lost" refers to the TCP connection, not the
 * event itself: the connection has been lost (dropped, closed, or errored) and
 * we are deciding whether to tell the ApplicationThread about it.
 *
 * DeliverLostEvent: the ApplicationThread receives a ConnectionLost EventMessage
 * so it can react (e.g. clear its ConnectionID, log a warning, schedule a retry).
 * This is the normal case for established connections that go down unexpectedly.
 *
 * SuppressLostEvent: the ApplicationThread is NOT notified. Used during
 * intentional teardown (e.g. epoll_ctl failure during setup, or when the
 * connection never reached the established state and the thread does not yet
 * hold a ConnectionID to act on).
 */
class DeliverLostEventFlag {
  public:
    enum DeliverLostEventFlagTag { SuppressLostEvent = 0, DeliverLostEvent = 1 };

    explicit DeliverLostEventFlag(DeliverLostEventFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const DeliverLostEventFlag& rhs) const {
        return value_ == rhs.value_;
    }

    [[nodiscard]] bool is_equal(const DeliverLostEventFlagTag& rhs) const {
        return value_ == rhs;
    }

    [[nodiscard]] DeliverLostEventFlagTag value() const {
        return value_;
    }

  private:
    DeliverLostEventFlagTag value_;
};

inline bool operator==(const DeliverLostEventFlag& lhs, const DeliverLostEventFlag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const DeliverLostEventFlag& lhs, const DeliverLostEventFlag::DeliverLostEventFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const DeliverLostEventFlag::DeliverLostEventFlagTag& lhs, const DeliverLostEventFlag& rhs) {
    return rhs.is_equal(lhs);
}

} // namespace pubsub_itc_fw
