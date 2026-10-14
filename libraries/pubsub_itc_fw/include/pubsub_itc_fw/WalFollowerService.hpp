#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>

#include <replication_messages.hpp>

namespace pubsub_itc_fw {

/// How WalFollowerService reaches the reading thread and the followers. Implemented by the owning thread.
class WalFollowerLinksInterface {
  public:
    virtual ~WalFollowerLinksInterface() = default;

    /// Hand @p command to the reading thread, through the framework's queue of messages between threads.
    virtual void send_to_reading_thread(std::unique_ptr<WalReadingCommand> command) = 0;
    virtual void send_declined(const ConnectionID& to, const WalFollowDeclined& declined) = 0;
    virtual void send_caught_up(const ConnectionID& to, const WalFollowCaughtUp& caught_up) = 0;
    virtual void send_leader_changed(const ConnectionID& to, const WalLeaderChanged& changed) = 0;
};

/// What WalFollowerService tells the owning thread. Called on the owning thread.
class WalFollowerEventsInterface {
  public:
    virtual ~WalFollowerEventsInterface() = default;

    /**
     * @brief A follower has been handed over: it holds every record up to @p through, and from now on
     *        the owning thread sends it each new record above that, as the record passes its limit.
     */
    virtual void follower_handed_over(const ConnectionID& follower, int64_t through) = 0;

    /// A follower's hand-over has fallen short three times in a row; see section 3.4 of the design.
    virtual void hand_over_falling_behind(const ConnectionID& follower) = 0;

    /// A follower asked while this instance does not lead, and was declined.
    virtual void declined_while_not_leading(const ConnectionID& follower) = 0;

    /// The reading thread has stopped sending to every follower, in answer to stop_all().
    virtual void reading_stopped() = 0;
};

/**
 * @brief Runs the following of a WAL from the thread that owns it: starts each follower's catch-up on the
 *        reading thread, and hands the follower over to new records at an exact position.
 *
 * The design is docs/availability/following_the_wal.md, sections 3.1 and 3.4. For each follower the
 * service keeps whether the reading thread is sending to it or the owning thread is. While the reading
 * thread is, the owning thread sends it nothing. When the reading thread reports that it has sent every
 * record up to the follower's limit as it last read it, the service compares that position with the
 * limit now:
 *
 * - **Equal:** nothing has been released since, so the follower is handed over. The service tells the
 *   follower it has caught up, if it asked to be told, and tells the application, which sends it each
 *   new record from then on.
 * - **Short:** records were released meanwhile, so the reading thread is told to carry on, and the
 *   round repeats. After three short rounds in a row the application is told, so that it can stop new
 *   records being released for long enough for the next round to end the hand-over.
 *
 * Nothing is missed or sent twice at the hand-over, because every message any thread sends goes
 * through the one queue into the reactor, in order: the reading thread queues its last record before
 * it reports, and the owning thread queues its first new record only after it has been told.
 *
 * **Threading.** Everything here runs on the owning thread, which also owns the WAL. The reading thread
 * is reached only through the links.
 */
class WalFollowerService {
  public:
    /// After this many short rounds in a row, the application is told the hand-over is falling behind.
    static constexpr int short_rounds_before_telling = 3;

    WalFollowerService(QuillLogger& logger, const Wal& wal, const WalFollowingPositions& positions, WalFollowerLinksInterface& links,
                       WalFollowerEventsInterface& events);

    /**
     * @brief A follower's request (WalFollowRequest) arrived on @p connection.
     *
     * @param[in] connection The connection the request arrived on, which the records are sent on.
     * @param[in] request    The request: the follower's last record, and whether it wants new records.
     * @param[in] leading   Whether this instance leads; one that does not declines.
     * @param[in] limit     How far this follower may be sent; the application decides by the connection.
     * @param[in] converter Which of the reading thread's converters turns records into its messages.
     */
    void on_follow_request(const ConnectionID& connection, const WalFollowRequestView& request, bool leading, FollowerLimit limit, int converter);

    /**
     * @brief Offers an inter-thread message to the service; true if it was a report from the reading
     *        thread, which the service has acted on.
     */
    [[nodiscard]] bool on_inter_thread_message(const EventMessage& message);

    /// The connection has gone: the reading thread stops sending to it, and the service forgets it.
    void on_connection_lost(const ConnectionID& connection);

    /// This instance has taken the lead: every follower it declined or was serving is told, and asks again.
    void on_became_leader(int32_t epoch);

    /// This instance has stopped leading: the reading thread stops sending to every follower.
    void stop_all();

    /// Whether the reading thread is sending to no follower, so that the WAL may be cut back.
    [[nodiscard]] bool reading_has_stopped() const {
        return !stop_pending_;
    }

    /// Whether @p connection has been handed over, so that the owning thread sends it new records.
    [[nodiscard]] bool is_handed_over(const ConnectionID& connection) const;

    /// The followers handed over, each with the limit it is served up to.
    [[nodiscard]] std::vector<std::pair<ConnectionID, FollowerLimit>> handed_over_followers() const;

  private:
    struct Follower {
        ConnectionID connection;
        FollowerLimit limit{FollowerLimit::LastCommitted};
        bool tell_when_caught_up{false};
        bool handed_over{false};
        int short_rounds{0};
    };

    void hand_over(Follower& follower, int64_t through, int64_t records_sent);

    QuillLogger& logger_;
    const Wal& wal_;
    const WalFollowingPositions& positions_;
    WalFollowerLinksInterface& links_;
    WalFollowerEventsInterface& events_;
    std::unordered_map<int, Follower> followers_;
    // Connections whose follower was declined, kept so that they are told when this instance leads.
    std::unordered_map<int, ConnectionID> declined_;
    bool stop_pending_{false};
};

} // namespaces
