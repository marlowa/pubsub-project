#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

namespace pubsub_itc_fw {

/**
 * @brief Whether something is waiting for a send to arrive, which decides where the send's timing is recorded.
 *
 * Most of what a component sends is routine: replication, streams to subscribers, acknowledgements, heartbeats. A few
 * sends are waited for: a request on its way to be answered, or the answer on its way back, with someone at the end
 * of the chain waiting for it. The two kinds are timed separately. Timed together, the many routine sends swamp the
 * few waited ones, and the figure describes neither: a component can then appear to spend longer on its part of a
 * request than the whole request takes.
 *
 * So each send says which it is. The class changes nothing about what is sent or where it goes; it decides only which
 * of two histograms records how long the send waited for its reactor and how long the reactor then took over it. Those
 * histograms carry the scope label `waited` or `routine`.
 *
 * A class rather than a bool because `send_pdu(conn, id, seq, msg, true)` tells a reader nothing, and this is a
 * decision a reader of the call site needs to be able to check.
 */
class SendLatencyClass {
  public:
    enum SendLatencyClassTag {
        Routine = 0, ///< Nothing waits for it: replication, subscriber streams, heartbeats, acknowledgements.
        Waited = 1   ///< Something waits for it: a request on its way to be answered, or the answer on its way back.
    };

    explicit SendLatencyClass(SendLatencyClassTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const SendLatencyClass& rhs) const {
        return value_ == rhs.value_;
    }

    [[nodiscard]] bool is_equal(const SendLatencyClassTag& rhs) const {
        return value_ == rhs;
    }

    [[nodiscard]] SendLatencyClassTag value() const {
        return value_;
    }

  private:
    SendLatencyClassTag value_;
};

inline bool operator==(const SendLatencyClass& lhs, const SendLatencyClass& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const SendLatencyClass& lhs, const SendLatencyClass::SendLatencyClassTag& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator==(const SendLatencyClass::SendLatencyClassTag& lhs, const SendLatencyClass& rhs) {
    return rhs.is_equal(lhs);
}

} // namespaces
