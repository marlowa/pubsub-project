#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ConnectionID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Tracks WAL cursors for connected external WAL subscribers.
 *
 * A cursor is the seq_no of the last WAL record a subscriber has
 * acknowledged. The publisher must not delete any WAL segment that
 * still contains records at or after any subscriber's cursor.
 *
 * min_cursor() returns the safe truncation boundary: the minimum cursor
 * across all currently connected subscribers. Only connected subscribers
 * participate -- a subscriber's cursor is removed immediately when its
 * connection is lost, so a crashed or disconnected subscriber cannot pin
 * WAL segments on disk indefinitely.
 *
 * Pre-emption rule: if a new connection arrives with the same subscriber_id
 * as an existing entry (e.g. a reconnect before the old TCP socket has been
 * torn down), register_subscriber() removes the old entry and returns its
 * ConnectionID. The caller must enqueue a ReactorControlCommand::Disconnect
 * for the returned ConnectionID to close the orphaned socket. The reactor
 * owns all socket I/O; callers must never close file descriptors directly.
 *
 * This class is not thread-safe. All calls must be made from the same
 * ApplicationThread (SequencerThread on the sequencer side).
 */
class ExternalWalSubscriberRegistry {
  public:
    /**
     * @brief Returned by min_cursor() when no subscribers are registered.
     *
     * Indicates that truncation is not bounded by any external subscriber.
     */
    static constexpr int64_t no_constraint = std::numeric_limits<int64_t>::max();

    /**
     * @brief Register a new external WAL subscriber.
     *
     * If a subscriber with the same subscriber_id is already registered
     * (orphan connection), its entry is removed and its ConnectionID is
     * returned. The caller must disconnect the orphaned connection via the
     * reactor. Returns an invalid ConnectionID (value == 0) if no orphan
     * exists.
     *
     * @param[in] connection_id   ConnectionID of the new connection.
     * @param[in] subscriber_id   Stable identity string (e.g. "mep_primary").
     * @param[in] initial_cursor  Starting cursor presented in WalSubscribeRequest.
     * @return ConnectionID of the displaced orphan, or ConnectionID{} if none.
     */
    ConnectionID register_subscriber(ConnectionID connection_id,
                                     const std::string& subscriber_id,
                                     int64_t initial_cursor) {
        ConnectionID orphan;
        auto identity_it = identity_to_connection_.find(subscriber_id);
        if (identity_it != identity_to_connection_.end()) {
            orphan = identity_it->second;
            cursors_.erase(orphan);
            identity_it->second = connection_id;
        } else {
            identity_to_connection_.emplace(subscriber_id, connection_id);
        }
        cursors_.emplace(connection_id, initial_cursor);
        return orphan;
    }

    /**
     * @brief Update the cursor for a connected subscriber (called on WalAck).
     *
     * @param[in] connection_id  ConnectionID of the subscriber.
     * @param[in] seq_no         Sequence number from the WalAck.
     * @return true if the connection was found and updated; false if unknown.
     */
    bool update_cursor(ConnectionID connection_id, int64_t seq_no) {
        auto it = cursors_.find(connection_id);
        if (it == cursors_.end()) {
            return false;
        }
        it->second = seq_no;
        return true;
    }

    /**
     * @brief Remove a subscriber on ConnectionLost.
     *
     * If connection_id is not registered this is a no-op.
     *
     * @param[in] connection_id  ConnectionID of the lost connection.
     */
    void remove_subscriber(ConnectionID connection_id) {
        if (cursors_.erase(connection_id) == 0) {
            return;
        }
        for (auto it = identity_to_connection_.begin(); it != identity_to_connection_.end(); ++it) {
            if (it->second == connection_id) {
                identity_to_connection_.erase(it);
                return;
            }
        }
    }

    /**
     * @brief Minimum cursor across all connected subscribers.
     *
     * Returns no_constraint if no subscribers are registered.
     */
    [[nodiscard]] int64_t min_cursor() const {
        int64_t minimum = no_constraint;
        for (const auto& entry : cursors_) {
            if (entry.second < minimum) {
                minimum = entry.second;
            }
        }
        return minimum;
    }

    /**
     * @brief Number of currently registered subscribers.
     */
    [[nodiscard]] size_t subscriber_count() const {
        return cursors_.size();
    }

  private:
    std::unordered_map<ConnectionID, int64_t> cursors_;                    // connection -> cursor
    std::unordered_map<std::string, ConnectionID> identity_to_connection_; // subscriber_id -> connection
};

} // namespaces
