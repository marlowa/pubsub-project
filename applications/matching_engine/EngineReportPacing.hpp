#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that EngineReportPacing can pause and resume the commands arriving,
 *        and learn when a connection for reports has written what was sent on it.
 */
class EngineReportPacingLinksInterface {
  public:
    virtual ~EngineReportPacingLinksInterface() = default;
    /// Stops reading every connection on which a sequencer sends this engine commands, so that TCP stops the sequencer
    /// sending more until reading resumes.
    virtual void pause_reading_commands() = 0;
    /// Reads every connection on which a sequencer sends this engine commands again.
    virtual void resume_reading_commands() = 0;
    /// Asks to be told, through EngineReportPacing::connection_written, when @p connection has nothing waiting to be
    /// written.
    virtual void request_written_notification(const pubsub_itc_fw::ConnectionID& connection) = 0;
};

/**
 * @brief Keeps the reports a matching engine sends no further ahead of what each connection for reports has written than
 *        a fixed number, by pausing the commands arriving while the connection to the leading sequencer catches up
 *        (docs/availability/pacing_the_engines_reports.md, BUG-0234).
 *
 * The framework closes a connection on which 65,536 sends are waiting to be written, as one whose peer has stopped
 * reading. An engine applying commands faster than the sequencer reads its reports, as one catching up does, would reach
 * that limit, and every report sent after the close would be lost. So for each connection for reports this counts the
 * reports sent and not yet known to be written. When the count reaches pause_at_reports it asks the connection to say
 * when it has nothing waiting. On the connection being paced, the one to the leading sequencer, it also pauses reading
 * the commands, so the engine applies no more until the connection has caught up; the sequencer, which sends only as fast
 * as the connection takes, then stops sending too. When the connection says it has nothing waiting, every report sent
 * before the request is written, and reading resumes.
 *
 * The connection to the other sequencer is counted the same way, so that what it has written is known, but it never
 * pauses anything: a slow follower sequencer must not slow the venue (principle V11).
 *
 * When a connection for reports is lost with reports on it not known to be written, this logs one Error naming the
 * sequencer, how many reports, and the commands they answer, instead of letting them go unremarked.
 *
 * One thread, the engine's, calls every member function.
 */
class EngineReportPacing {
  public:
    /// The most reports sent on a connection and not known to be written before the engine stops applying commands, far
    /// below the framework's limit of 65,536 waiting sends, so that the commands already read when a pause takes effect
    /// cannot carry the connection to that limit.
    static constexpr int64_t pause_at_reports = 4096;

    /**
     * @param[in] links  How the commands are paused and resumed, and notification asked for.
     * @param[in] logger Where a loss is logged.
     */
    EngineReportPacing(EngineReportPacingLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// The connection for reports to the leading sequencer, which is the one paced on, or an invalid one for none.
    void pace_on(const pubsub_itc_fw::ConnectionID& connection);

    /// A report answering command @p seq_no has just been sent on @p connection.
    void report_sent(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no);

    /// @p connection has said it has nothing waiting to be written, as asked.
    void connection_written(const pubsub_itc_fw::ConnectionID& connection);

    /// @p connection, a connection for reports to the sequencer named @p sequencer_name, is lost.
    void connection_lost(const pubsub_itc_fw::ConnectionID& connection, const std::string& sequencer_name);

    /// Whether reading the commands is paused now.
    [[nodiscard]] bool paused() const {
        return paused_;
    }

    /// How many reports sent on @p connection are not known to be written.
    [[nodiscard]] int64_t not_known_written(const pubsub_itc_fw::ConnectionID& connection) const;

  private:
    // Reports sent on one connection: a count, and the lowest and highest command they answer.
    struct Reports {
        int64_t count{0};
        int64_t lowest_seq_no{0};
        int64_t highest_seq_no{0};

        void add(int64_t seq_no);
        void add(const Reports& other);
    };

    // What is known of one connection for reports.
    struct Connection {
        pubsub_itc_fw::ConnectionID id{};
        // Sent before the notification now asked for was asked for: written once it arrives.
        Reports before_request{};
        // Sent since then, or since the last notification when none is asked for.
        Reports since_request{};
        bool notification_asked{false};
    };

    [[nodiscard]] Connection* find(const pubsub_itc_fw::ConnectionID& connection);
    [[nodiscard]] const Connection* find(const pubsub_itc_fw::ConnectionID& connection) const;
    [[nodiscard]] Connection& find_or_add(const pubsub_itc_fw::ConnectionID& connection);
    // Asks for notification on @p connection if enough reports wait on it, and pauses if it is the one paced on.
    void ask_if_needed(Connection& connection);
    void pause();
    void resume();

    EngineReportPacingLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    // A connection to each sequencer at most.
    std::array<Connection, 2> connections_{};
    pubsub_itc_fw::ConnectionID paced_{};
    bool paused_{false};
};

} // namespaces
