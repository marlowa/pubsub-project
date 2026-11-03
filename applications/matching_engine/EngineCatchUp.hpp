#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <string>
#include <unordered_set>

#include <pubsub_itc_fw/CatchUpTally.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <replication_messages.hpp>

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that EngineCatchUp can ask to be caught up and act on the answer.
 */
class EngineCatchUpLinksInterface {
  public:
    virtual ~EngineCatchUpLinksInterface() = default;

    /// Sends @p request on @p connection, a sequencer's connection for orders to this engine.
    virtual void send_follow_request(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequest& request) = 0;

    /// Starts the timer that asks again while a catch-up is not answered; its ticks are given to on_retry_tick().
    virtual void start_retry_timer() = 0;
    /// Stops that timer.
    virtual void stop_retry_timer() = 0;

    /// Asks every sequencer to halt trading, because this engine cannot account for the orders the venue holds.
    virtual void request_halt() = 0;
    /// Starts the timer that asks again, every second, while the halt is awaited; its ticks are given to
    /// on_halt_tick().
    virtual void start_halt_timer() = 0;
    /// Stops that timer.
    virtual void stop_halt_timer() = 0;

    /// The test event RegionRebuilt: the book rebuilt from the WAL, through @p through, is the venue's.
    virtual void region_rebuilt(int64_t through) = 0;
};

/**
 * @brief Asking the leading sequencer to be caught up from its WAL, and checking that the answer was complete before
 *        the engine acts on it (R-0101; docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.4).
 *
 * **A catch-up.** The engine sends a request (`WalFollowRequest`) on every sequencer connection for orders, with a new
 * number and the last command it has applied. Only the leading sequencer answers: it says where its answer starts
 * (`WalFollowStarted`), sends every record after that position, and says when it has sent them all
 * (`WalFollowCaughtUp`). The engine applies only the answer to its latest request, on the connection that answer
 * started on: a record from an earlier answer can still be on its way. A tally of the records applied must account for
 * every record the sequencer says it sent; if it does not, the catch-up is not acted on, and the request is made again.
 *
 * **Asking again.** A sequencer that does not lead declines and says when it leads (`WalLeaderChanged`), which makes the
 * engine ask again at once. A retry timer asks again when nothing has arrived since its last tick, but not while
 * records are arriving: asking a sequencer that is answering would have it send the whole catch-up a second time. No
 * catch-up is asked for while the question of a command the previous process died applying is open, because the
 * catch-up would apply that command again.
 *
 * **The engine's position**: the last command its open-order region recorded as applied when the process found it, or
 * none when there was no region. With none, the engine asks to be placed at the sequencer's head rather than sent the
 * whole day.
 *
 * **An unusable region.** When the region the process found could not be read, the engine asks for every record from
 * the first. If the WAL reaches the first command of the trading day, the rebuilt book is the venue's, and the region
 * counts as usable from then on. Otherwise the engine cannot account for what the venue holds: if it is to lead, or high
 * availability is off, it asks the sequencers every second to halt trading, until trading is halted or it stops
 * leading (R-0123).
 *
 * Whether the engine is catching up, and for what purpose, is its role's to say, not this part's: the thread passes it
 * in where it matters.
 *
 * One thread, the engine's, calls every member function.
 */
class EngineCatchUp {
  public:
    /// What a request says that is not this part's to know.
    struct RequestContext {
        /// Whether a catch-up may be asked for now: false while the question of a command the previous process died
        /// applying is open.
        bool may_ask{true};
        /// That command, for the Quill log, while the question is open.
        int64_t died_applying{0};
        /// Whether the engine asks as one that is to lead, rather than to follow the WAL.
        bool to_lead{false};
        /// The last command the engine has applied, however it applied it.
        int64_t highest_applied{0};
    };

    /// What became of a WalFollowCaughtUp.
    enum class Answer {
        other_request, ///< it answers an earlier request, and is ignored
        incomplete,    ///< the tally does not account for every record sent; asked for again on the next tick
        complete       ///< the catch-up is complete
    };

    /**
     * @param[in] instance_id                   This engine's instance, which names it in its requests.
     * @param[in] ha_enabled                    Whether high availability is on.
     * @param[in] sequencer_order_connections   The sequencers' connections for orders to this engine, which the thread
     *                                          keeps; requests are sent on each.
     * @param[in] links                         How requests are sent and the timers started.
     * @param[in] logger                        Where what happens is logged.
     */
    EngineCatchUp(int64_t instance_id, bool ha_enabled, const std::unordered_set<pubsub_itc_fw::ConnectionID>& sequencer_order_connections,
                  EngineCatchUpLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    // ---- The engine's position, from the region the process found -------------------------------------------------

    /// The region the process found could not be read.
    void region_found_unusable() {
        region_was_unusable_ = true;
        has_position_ = true;
        position_found_ = 0;
    }
    /// No region was left behind: the engine has no position of its own.
    void no_region_found() {
        has_position_ = false;
    }
    /// The region was read, and was current to command @p published.
    void region_found_current_to(int64_t published) {
        has_position_ = true;
        position_found_ = published;
    }
    /// Whether the region could not be read, and the catch-up has not since shown that the rebuilt book is the venue's.
    [[nodiscard]] bool region_was_unusable() const {
        return region_was_unusable_;
    }
    /// The last command the region vouched for, or the last command the latest complete catch-up reached.
    [[nodiscard]] int64_t position() const {
        return position_found_;
    }

    // ---- A catch-up ------------------------------------------------------------------------------------------------

    /**
     * @brief A catch-up begins: the counts are set to zero, the request sent if a sequencer is connected for orders, and
     *        the retry timer started.
     * @return Whether the request was sent.
     */
    bool begin(const RequestContext& context);

    /**
     * @brief Sends a new request, with a new number, on every sequencer connection for orders.
     * @return Whether it was sent: not while the died-applying question is open, or with no connection.
     */
    bool send_request(const RequestContext& context);

    /// Whether the request of the present catch-up has been sent on at least one connection.
    [[nodiscard]] bool request_sent() const {
        return request_sent_;
    }

    /// Sends the latest request again, with the same number, on @p connection alone, so that an answer under way on
    /// another connection is left to finish (BUG-0200).
    void send_request_on(const pubsub_itc_fw::ConnectionID& connection, const RequestContext& context);

    /// Whether a record arriving on @p connection, while catching up, is from an earlier answer, and so ignored.
    [[nodiscard]] bool from_an_earlier_answer(const pubsub_itc_fw::ConnectionID& connection) const {
        return connection != answer_connection_;
    }
    /// A record from an earlier answer was ignored.
    void ignored_a_record_from_an_earlier_answer() {
        ++records_from_earlier_answers_;
    }
    /// The record numbered @p seq_no, of the answer, has been offered to the engine, applied or not.
    void record_offered(int64_t seq_no) {
        ++records_seen_;
        tally_.offer(seq_no);
    }

    /**
     * @brief `WalFollowStarted` has arrived on @p connection.
     * @param[in] catching_up Whether the engine is catching up.
     * @return Whether it starts the answer to the latest request, which is then the answer applied.
     */
    bool started(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowStartedView& started, bool catching_up);

    /**
     * @brief `WalFollowCaughtUp` has arrived on @p connection while the engine is catching up.
     * @param[in] book_size The number of orders on the book, for the Quill log.
     * @return What became of it. When it is Answer::complete, the retry timer is stopped and the position is the
     *         last record it names.
     */
    Answer caught_up(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowCaughtUpView& ack, size_t book_size);

    /// A tick of the retry timer. @p catching_up says whether the engine is still catching up; if it is not, the timer
    /// is stopped.
    void on_retry_tick(bool catching_up, const RequestContext& context);

    // ---- Reports produced by a catch-up ----------------------------------------------------------------------------

    /// The last command the WAL holds every report of, as the latest answer said when it started, or 0. A catch-up
    /// sends no report of a command at or before it.
    [[nodiscard]] int64_t reports_in_wal_through() const {
        return reports_in_wal_through_;
    }
    void report_sent() {
        ++reports_sent_;
    }
    void report_left_out() {
        ++reports_left_out_;
    }

    // ---- Following the WAL after a catch-up ------------------------------------------------------------------------

    /// From now on the engine follows the WAL, and the next record must be numbered @p first.
    void follow_from(int64_t first) {
        next_followed_seq_no_ = first;
    }
    /// The engine no longer expects a particular record: a catch-up to follow again has begun.
    void stop_following() {
        next_followed_seq_no_ = 0;
    }
    /// The record the engine expects next while following, or 0 before the catch-up has ended.
    [[nodiscard]] int64_t next_followed() const {
        return next_followed_seq_no_;
    }

    /**
     * @brief Whether the record numbered @p seq_no, arriving while following, is the next one, which is then expected
     *        no more. Before the catch-up has ended, every record is still its answer, and is accepted.
     */
    [[nodiscard]] bool next_followed_record(int64_t seq_no);

    // ---- An unusable region ----------------------------------------------------------------------------------------

    /**
     * @brief Decides, once a catch-up of an engine whose region could not be read has completed, what follows (R-0123).
     * @param[in] ack                 The answer that completed it.
     * @param[in] to_lead_or_alone    Whether the engine is catching up to lead, or high availability is off.
     * @param[in] book_size           The number of orders on the book, for the Quill log.
     */
    void account_for_unusable_region(const pubsub_itc_fw::WalFollowCaughtUpView& ack, bool to_lead_or_alone, size_t book_size);

    /// Whether the engine waits for the halt it asked for.
    [[nodiscard]] bool halt_awaited() const {
        return halt_awaited_;
    }
    /// Trading is halted, or the engine has stopped leading: the halt is no longer asked for.
    void stop_awaiting_halt();
    /// A tick of the halt timer: asked again unless @p halted.
    void on_halt_tick(bool halted);

    // ---- Counts, for the Quill log and the test --------------------------------------------------------------------

    [[nodiscard]] bool has_position() const {
        return has_position_;
    }
    [[nodiscard]] int64_t records_seen() const {
        return records_seen_;
    }
    [[nodiscard]] int64_t reports_sent() const {
        return reports_sent_;
    }
    [[nodiscard]] int64_t reports_left_out() const {
        return reports_left_out_;
    }
    /// What the sequencer said it had sent, in the catch-up that last completed.
    [[nodiscard]] int64_t last_records_sent() const {
        return last_records_sent_;
    }
    [[nodiscard]] int64_t request_number() const {
        return request_number_;
    }

  private:
    // The request this engine sends, naming it follower_name, which must outlive the request.
    [[nodiscard]] pubsub_itc_fw::WalFollowRequest request(const std::string& follower_name, const RequestContext& context) const;

    const int64_t instance_id_;
    const bool ha_enabled_;
    const std::unordered_set<pubsub_itc_fw::ConnectionID>& sequencer_order_connections_;
    EngineCatchUpLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    // The number of the latest request, counting up from 1. The same number is sent on every connection, and the
    // leading sequencer echoes it on WalFollowStarted and WalFollowCaughtUp.
    int64_t request_number_{0};
    // Whether the present catch-up has been asked for on at least one connection: false from the moment a catch-up
    // begins until the request is first sent, which is later when no sequencer had connected yet.
    bool request_sent_{false};
    // The connection the answer to the latest request started on, or none until it has.
    pubsub_itc_fw::ConnectionID answer_connection_{};
    // Accounts for the records of the answer to the latest request, and is set again when that answer starts.
    pubsub_itc_fw::CatchUpTally tally_{0};

    // For the present catch-up: ticks of the retry timer; records offered, and the figure at the previous tick, so that
    // a request is repeated only when nothing has arrived since the last; reports sent and left out; and records from
    // earlier answers ignored.
    int attempts_{0};
    int64_t records_seen_{0};
    int64_t records_at_last_tick_{0};
    int64_t reports_sent_{0};
    int64_t reports_left_out_{0};
    int64_t reports_in_wal_through_{0};
    int64_t records_from_earlier_answers_{0};
    int64_t last_records_sent_{0};

    // Whether the engine knows where it had got to: a process that found a region does; one that found none does not,
    // and must not pretend the answer is zero, which would ask for the whole retained WAL.
    bool has_position_{false};
    int64_t position_found_{0};
    // While following, after the catch-up: the number the next record must have, or 0 when none is expected.
    int64_t next_followed_seq_no_{0};

    bool region_was_unusable_{false};
    bool halt_awaited_{false};
};

} // namespaces
