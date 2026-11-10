#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <unordered_map>

namespace pubsub_itc_fw {

/** @ingroup reactor_subsystem */

/**
 * @brief Counts the sends that arrive for connections that have gone, so that they can be reported as one line for each
 *        connection rather than one line for each send.
 *
 * A thread that queued many sends for a connection that then closed would otherwise write a Warning for every one of them,
 * which can fill a disk in minutes. Used only on the reactor thread.
 */
class SendsToGoneConnections {
  public:
    /**
     * @brief Counts one send dropped because its connection has gone.
     * @param [in] connection_id The connection the send was for.
     */
    void count_send(int connection_id) {
        ++sends_by_connection_[connection_id];
    }

    /**
     * @brief Reports each connection counted since the last call, once, and forgets them.
     * @tparam Report A callable taking the connection identifier as int and the number of sends as int64_t.
     * @param [in] report Called once for each connection.
     */
    template <typename Report> void report_and_forget(const Report& report) {
        for (const auto& [connection_id, sends] : sends_by_connection_) {
            report(connection_id, sends);
        }
        sends_by_connection_.clear();
    }

  private:
    std::unordered_map<int, int64_t> sends_by_connection_;
};

} // namespaces
