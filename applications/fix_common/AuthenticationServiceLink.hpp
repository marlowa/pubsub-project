#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace fix_common {

/**
 * @brief What an order gateway knows about its connection to one instance of the authentication service: whether it
 *        has one, whether the service is answering on it, and what to do when the heartbeat timer fires.
 *
 * A gateway holds one of these for each authentication service instance it connects to, primary and, with high
 * availability, secondary. Both order gateways use it, so that they keep the connection open and judge it in the same
 * way.
 *
 * **Why the connection needs a heartbeat.** After a member has logged on, a gateway sends nothing to the
 * authentication service until the next logon. The service closes any connection on which it has received nothing for
 * its inactivity limit. So without a heartbeat the connection is closed on a healthy venue, and a logon that arrives
 * before the gateway has connected again is refused for want of an authentication service.
 *
 * **Why the heartbeat is answered.** The gateway must also notice when the service stops answering while the
 * connection stays open, as happens when the service's process hangs or its machine disappears without closing the
 * socket. Nothing else tells the gateway: the connection still looks open, so every logon would be sent to that
 * service and wait for the whole authentication timeout before being refused, and none would go to the other instance.
 * The service answers each heartbeat, and anything at all received on the connection counts as an answer.
 *
 * **The states of a connection.**
 * - Not connected: there is no connection, or the last one has been lost.
 * - Waiting for the first answer: the connection has just been made and nothing has yet arrived on it. It is not used
 *   for logons. The operating system completes a connection to a process that is stopped but still holds its
 *   listening socket, so a connection being made says nothing about whether the service will answer on it.
 * - Answering: something has arrived on the connection, so the service is answering. Logons may be sent on it.
 * - Being closed: nothing arrived for longer than the silence limit, and the gateway has asked for the connection to
 *   be closed and made again. It is not used for logons, and no heartbeats are sent on it.
 *
 * **The silence limit** is three heartbeat intervals. The gateway checks on each heartbeat, so a silent connection is
 * closed between three and four intervals after the last thing arrived on it. Three rather than one is so that a
 * heartbeat answered late, because the service was busy with a burst of logons, does not close a connection that is
 * working.
 *
 * **Time.** The caller passes the time to every call, from std::chrono::steady_clock, so that a test can say exactly
 * when each thing happens. The class never reads a clock.
 *
 * **Threading.** Not thread-safe. A gateway uses it from its one gateway thread.
 */
class AuthenticationServiceLink {
  public:
    using Clock = std::chrono::steady_clock;

    /// How many heartbeat intervals may pass with nothing received before a connection is judged silent.
    static constexpr int heartbeat_intervals_before_silent = 3;

    /// The state of the connection, as described in the class comment.
    enum class State { NotConnected, WaitingForFirstAnswer, Answering, BeingClosed };

    /// What the gateway is to do with this connection when the heartbeat timer fires.
    enum class HeartbeatAction {
        Nothing,       ///< There is no connection, or it is already being closed.
        SendHeartbeat, ///< Send an AuthenticationHeartbeat on the connection.
        CloseAsSilent  ///< Nothing has arrived for longer than the silence limit: close the connection and connect again.
    };

    /// How a connection came to be lost, so that the gateway can say so at the right level.
    enum class LossCause {
        NotThisConnection, ///< The connection lost is not this one.
        ClosedByPeer,      ///< Closed by the service or by the network, which needs looking into.
        ClosedForSilence   ///< Closed by the gateway because the service had stopped answering, which was already reported.
    };

    ~AuthenticationServiceLink() = default;

    /**
     * @brief Creates a link with no connection.
     *
     * @param[in] heartbeat_interval How often the gateway's heartbeat timer fires. Must be greater than zero.
     */
    explicit AuthenticationServiceLink(std::chrono::milliseconds heartbeat_interval) : heartbeat_interval_(validated(heartbeat_interval)) {}

    AuthenticationServiceLink(const AuthenticationServiceLink& other) = default;
    AuthenticationServiceLink& operator=(const AuthenticationServiceLink& other) = default;
    AuthenticationServiceLink(AuthenticationServiceLink&& other) = default;
    AuthenticationServiceLink& operator=(AuthenticationServiceLink&& other) = default;

    /**
     * @brief A connection to the service has been made.
     *
     * The connection is not used for logons until something arrives on it. The gateway should send a heartbeat on it
     * at once, so that it is proved within one round trip rather than at the next heartbeat.
     *
     * @param[in] connection_id The new connection.
     * @param[in] now           When it was made.
     */
    void on_established(const pubsub_itc_fw::ConnectionID& connection_id, Clock::time_point now) {
        connection_id_ = connection_id;
        state_ = State::WaitingForFirstAnswer;
        last_heard_ = now;
    }

    /**
     * @brief Whether @p connection_id is this link's connection, open or being closed.
     *
     * @param[in] connection_id The connection a message or event arrived on.
     */
    [[nodiscard]] bool owns(const pubsub_itc_fw::ConnectionID& connection_id) const {
        return state_ != State::NotConnected && connection_id_ == connection_id;
    }

    /**
     * @brief Something arrived on this link's connection: a heartbeat answer, or part of an authentication exchange.
     *
     * @param[in] now When it arrived.
     * @return true when this is the first thing to arrive on the connection, so that the connection has just become
     *         usable for logons and the gateway should say so; false otherwise.
     */
    [[nodiscard]] bool on_heard(Clock::time_point now) {
        if (state_ == State::NotConnected) {
            throw pubsub_itc_fw::PreconditionAssertion("AuthenticationServiceLink::on_heard: there is no connection", __FILE__, __LINE__);
        }
        last_heard_ = now;
        if (state_ != State::WaitingForFirstAnswer) {
            return false;
        }
        state_ = State::Answering;
        return true;
    }

    /**
     * @brief A connection was lost. If it is this link's, the link now has no connection.
     *
     * @param[in] connection_id The connection lost.
     * @return How it came to be lost, or LossCause::NotThisConnection if it is not this link's connection.
     */
    [[nodiscard]] LossCause on_lost(const pubsub_itc_fw::ConnectionID& connection_id) {
        if (!owns(connection_id)) {
            return LossCause::NotThisConnection;
        }
        const LossCause cause = state_ == State::BeingClosed ? LossCause::ClosedForSilence : LossCause::ClosedByPeer;
        state_ = State::NotConnected;
        connection_id_ = pubsub_itc_fw::ConnectionID{};
        return cause;
    }

    /**
     * @brief The heartbeat timer fired: says what to do with this link's connection.
     *
     * When the answer is HeartbeatAction::CloseAsSilent the link is then being closed, and the gateway must ask for the
     * connection to be closed and connected again.
     *
     * @param[in] now When the timer fired.
     */
    [[nodiscard]] HeartbeatAction on_heartbeat_timer(Clock::time_point now) {
        if (state_ == State::NotConnected || state_ == State::BeingClosed) {
            return HeartbeatAction::Nothing;
        }
        if (now - last_heard_ > silence_limit()) {
            state_ = State::BeingClosed;
            return HeartbeatAction::CloseAsSilent;
        }
        return HeartbeatAction::SendHeartbeat;
    }

    /// Whether logons may be sent on this link's connection: something has arrived on it, and it is not being closed.
    [[nodiscard]] bool is_usable() const {
        return state_ == State::Answering;
    }

    /// The state of the connection.
    [[nodiscard]] State state() const {
        return state_;
    }

    /// This link's connection; not valid when there is none.
    [[nodiscard]] const pubsub_itc_fw::ConnectionID& connection_id() const {
        return connection_id_;
    }

    /// How long may pass with nothing received before the connection is judged silent.
    [[nodiscard]] Clock::duration silence_limit() const {
        return heartbeat_interval_ * heartbeat_intervals_before_silent;
    }

    /// When something last arrived on the connection, or when it was made if nothing has.
    [[nodiscard]] Clock::time_point last_heard() const {
        return last_heard_;
    }

  private:
    static std::chrono::milliseconds validated(std::chrono::milliseconds heartbeat_interval) {
        if (heartbeat_interval <= std::chrono::milliseconds::zero()) {
            throw pubsub_itc_fw::PreconditionAssertion("AuthenticationServiceLink: the heartbeat interval must be greater than zero", __FILE__, __LINE__);
        }
        return heartbeat_interval;
    }

    std::chrono::milliseconds heartbeat_interval_;
    State state_{State::NotConnected};
    pubsub_itc_fw::ConnectionID connection_id_{};
    Clock::time_point last_heard_{};
};

} // namespaces
