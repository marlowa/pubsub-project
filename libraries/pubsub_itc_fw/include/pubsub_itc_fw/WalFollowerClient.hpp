#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <replication_messages.hpp>

namespace pubsub_itc_fw {

/// How WalFollowerClient sends its requests. Implemented by the follower's thread.
class WalFollowerClientLinksInterface {
  public:
    virtual ~WalFollowerClientLinksInterface() = default;
    virtual void send_follow_request(const ConnectionID& to, const WalFollowRequest& request) = 0;
};

/**
 * @brief The side of a component that follows a WAL: asks to be sent the records it lacks, checks what
 *        it is sent, and keeps the last record it holds.
 *
 * The design is docs/availability/following_the_wal.md, sections 3.1 and 3.6. The follower cannot tell
 * which instance of the pair leads, so it asks both; the one that does not lead declines, and tells it
 * if it later takes the lead, when it asks again. The application passes on each message the library
 * sends, and the sequence number of each record it is sent; the client says what to do with each record:
 *
 * - **Accept:** the next record after the last held. The application applies it.
 * - **Already held:** at or below the last held, as can happen when a request is made again. Dropped.
 * - **Gap:** beyond the next. The client asks again from the last record held, and the application drops
 *   the record; a gap means records were lost on the way, and is reported in the Quill log by the caller.
 *
 * When told it has caught up, the client checks that it received exactly as many records as the leading
 * instance says it sent since the request (requirement R-0101 for the matching engine); if not, it asks
 * again, and does not count itself caught up.
 *
 * Each request carries a number, counting up from 1, which the leading instance echoes on WalFollowStarted,
 * sent before the first record of its answer, and on WalFollowCaughtUp. A record that arrives on a connection
 * before the WalFollowStarted for the latest request sent on that connection is from an earlier answer still on
 * its way: it is not counted, and is dropped. A WalFollowCaughtUp for an earlier request is ignored.
 *
 * **Threading.** Called only on the follower's thread.
 */
class WalFollowerClient {
  public:
    enum class RecordVerdict : int8_t { Accept, AlreadyHeld, Gap, ForAnEarlierRequest };
    enum class CaughtUpVerdict : int8_t { CaughtUp, CountMismatch, ForAnEarlierRequest };

    /**
     * @param[in] follower_name     Who is asking, for the Quill log of the instance asked.
     * @param[in] wants_new_records Whether, once caught up, it wants new records as they come.
     * @param[in] last_seq_no       The last record it already holds; -1 if it holds nothing and wants
     *                              to be placed at the newest record, sent nothing older.
     * @param[in] links             How requests are sent.
     */
    WalFollowerClient(std::string follower_name, bool wants_new_records, int64_t last_seq_no, WalFollowerClientLinksInterface& links);

    /// A connection to an instance of the pair is up: ask it.
    void on_connected(const ConnectionID& connection);

    /// A connection has gone. Anything counted on it is forgotten; a new connection asks afresh.
    void on_disconnected(const ConnectionID& connection);

    /// The instance on @p connection does not lead. Nothing to do until it says it leads.
    void on_declined(const ConnectionID& connection);

    /// The instance on @p connection has taken the lead: ask it again.
    void on_leader_changed(const ConnectionID& connection);

    /// The instance on @p connection has started to answer a request.
    void on_started(const ConnectionID& connection, const WalFollowStartedView& started);

    /// A record numbered @p seq_no arrived on @p connection.
    [[nodiscard]] RecordVerdict on_record(const ConnectionID& connection, int64_t seq_no);

    /// The instance on @p connection says it has sent everything.
    [[nodiscard]] CaughtUpVerdict on_caught_up(const ConnectionID& connection, const WalFollowCaughtUpView& caught_up);

    /// The last record held, or -1 if none is yet.
    [[nodiscard]] int64_t last_seq_no() const {
        return last_seq_no_;
    }

    /// Whether the follower has caught up since it last asked.
    [[nodiscard]] bool caught_up() const {
        return caught_up_;
    }

    /// The earliest record the leading instance's WAL held when the follower caught up, or zero if none.
    [[nodiscard]] int64_t first_seq_no_held() const {
        return first_seq_no_held_;
    }

  private:
    void ask(const ConnectionID& connection);

    // The number of the latest request sent on @p connection, or 0 if none has been.
    [[nodiscard]] int64_t latest_request_on(const ConnectionID& connection) const;

    std::string follower_name_;
    bool wants_new_records_;
    int64_t last_seq_no_;
    WalFollowerClientLinksInterface& links_;
    // Records received on each connection since the instance started to answer the latest request, as the instance
    // counts them. A connection is in the map only once that answer has started.
    std::unordered_map<int, int64_t> received_since_request_;
    // The number of the latest request sent on each connection. Each request is numbered one more than the last one sent
    // on any connection.
    std::unordered_map<int, int64_t> request_numbers_;
    int64_t last_request_number_{0};
    bool caught_up_{false};
    int64_t first_seq_no_held_{0};
};

} // namespaces
