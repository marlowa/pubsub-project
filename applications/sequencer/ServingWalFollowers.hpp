#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReplicatedWal.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowerService.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/WalHandedOverFollowers.hpp>

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that ServingWalFollowers can send messages, tell the replicated WAL and
 *        the other parts how a catch-up went, and route orders to an engine caught up to lead.
 *
 * It includes the library's WalFollowerLinksInterface, which the reading thread's service uses to send its commands
 * and messages.
 */
class ServingWalFollowersLinksInterface : public pubsub_itc_fw::WalFollowerLinksInterface {
  public:
    /// The connection the replicated WAL sends the follower sequencer its records on, invalid if there is none.
    [[nodiscard]] virtual pubsub_itc_fw::ConnectionID peer_connection() const = 0;
    /// Whether @p connection is a matching engine publisher's.
    [[nodiscard]] virtual bool is_publisher_connection(const pubsub_itc_fw::ConnectionID& connection) const = 0;
    /// The earliest record the WAL holds.
    [[nodiscard]] virtual int64_t first_seq_no_held() const = 0;

    /// Publishes the last order released as the limit a matching engine is served up to.
    virtual void publish_released_position() = 0;
    /// Pauses reading from the gateways until a follower's hand-over finishes (OrderRelease::pause_for_hand_over).
    virtual void pause_for_hand_over() = 0;
    /// A hand-over has finished, or will not (OrderRelease::end_pause_for_hand_over).
    virtual void end_pause_for_hand_over() = 0;
    /// The follower sequencer holds every record through @p through; the replicated WAL sends it the rest.
    virtual void peer_caught_up(int64_t through) = 0;
    /// The reading thread has stopped serving the follower sequencer.
    virtual void peer_catch_up_stopped() = 0;
    /// The matching engine on @p connection, which asked to be caught up, has been sent records through @p sent_through.
    virtual void engine_catch_up_progress(const pubsub_itc_fw::ConnectionID& connection, int64_t sent_through) = 0;
    /// The engine on @p connection has been handed over: its catch-up is no longer measured (EnginePresence::catch_up_ended).
    virtual void engine_catch_up_ended(const pubsub_itc_fw::ConnectionID& connection) = 0;
    /// The test event EngineHandedOver.
    virtual void engine_handed_over(const pubsub_itc_fw::ConnectionID& connection, int64_t through, int64_t records_sent, bool answers_catch_up_request) = 0;
    /// The engine on @p connection is caught up to lead: orders are routed to it, and those held go to it.
    virtual void engine_caught_up_to_lead(const pubsub_itc_fw::ConnectionID& connection) = 0;
    /// Sets the gauge of the damaged record that stops a follower being caught up, 0 when there is none.
    virtual void set_damaged_record_gauge(int64_t seq_no) = 0;
};

/**
 * @brief What this instance does for those that follow its WAL: the follower sequencer's catch-up, the matching engine
 *        publishers and the matching engines that follow the WAL, once handed over, and the damaged records that stop one
 *        being caught up (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.15).
 *
 * The library's WalFollowerService catches each follower up on the reading thread, and hands it over to this thread
 * once it holds every record up to its limit. Once handed over, a publisher is sent each record as it is committed, and
 * a matching engine that follows the WAL each record as it is released; the follower sequencer is sent its records by
 * the replicated WAL. Only the leader serves followers: when this instance stops leading, every follower being served
 * or handed over is stopped, and each asks again of whichever instance leads next.
 *
 * **A matching engine being served** is one of three kinds:
 *
 * | Kind | Served because | When handed over |
 * |------|----------------|------------------|
 * | sent the orders it lacks | this instance took the lead and the engine said it lacks orders the WAL holds, or orders waited in the WAL | nothing more:
 * orders go to it as before | | caught up to lead | it asked to be caught up, saying it is to lead | WalFollowCaughtUp sent; orders routed to it | | caught up
 * to follow | it asked to be caught up, saying it is not to lead | WalFollowCaughtUp sent; sent every record as it is released from then on |
 *
 * While an engine is being served, this instance sends it nothing itself: the reading thread sends it everything up to
 * the last order released.
 *
 * **A damaged record** that stops a follower being caught up is said at Error the first time it stops each kind of
 * follower, and at Info when the same follower asks again, and is shown by a gauge holding its number until a follower
 * is handed over past it.
 *
 * One thread, the sequencer's, calls every member function.
 */
class ServingWalFollowers : public pubsub_itc_fw::PeerCatchUpInterface, public pubsub_itc_fw::WalFollowerEventsInterface {
  public:
    /// The reading thread's converters, by number.
    struct Converters {
        int peer;      ///< each record as a ReplicatedRecord, for the follower sequencer
        int engine;    ///< each order and cancel, for a matching engine to lead
        int publisher; ///< every record as the WAL holds it, for a publisher and a matching engine that follows the WAL
    };

    /// The kinds of matching engine being served.
    enum class EngineKind { sent_the_orders_it_lacks, caught_up_to_lead, caught_up_to_follow };

    /// A matching engine being served, and why.
    struct EngineServing {
        EngineKind kind{EngineKind::sent_the_orders_it_lacks};
        /// For an engine that asked to be caught up: the start of the answer, which the reading thread sends before the
        /// first record, and whose request number WalFollowCaughtUp echoes.
        std::optional<pubsub_itc_fw::WalFollowStart> started;
    };

    ServingWalFollowers(Converters converters, ServingWalFollowersLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /**
     * @brief Makes the reading thread's service and the stores of followers handed over, once the WAL is open.
     * @param[in] publisher_links How a record is sent to a publisher or an engine that follows the WAL, once handed over.
     * @param[in] kept            How many records written are kept to send to them without reading them back.
     */
    void start(const pubsub_itc_fw::Wal& wal, const std::string& wal_directory, const pubsub_itc_fw::WalFollowingPositions& positions,
               pubsub_itc_fw::WalHandedOverFollowersLinksInterface& publisher_links, pubsub_itc_fw::WalKeptRecordsCapacity kept);

    // ---- Events from the thread ------------------------------------------------------------------------------------

    /// A message from the reading thread. @return Whether it was one.
    [[nodiscard]] bool on_inter_thread_message(const pubsub_itc_fw::EventMessage& message);
    /// The thread has written record @p seq_no as the @p size bytes at @p bytes, a message of type @p pdu_id.
    void written(int64_t seq_no, int16_t pdu_id, const uint8_t* bytes, size_t size);
    /// A matching engine's connection is lost.
    void engine_connection_lost(const pubsub_itc_fw::ConnectionID& connection);
    /// A publisher's connection is lost.
    void publisher_connection_lost(const pubsub_itc_fw::ConnectionID& connection);
    /// This instance has taken the lead at @p epoch: every engine declined while it followed is told to ask again.
    void became_leader(int32_t epoch);
    /// This instance has stopped leading: every follower being served or handed over is stopped.
    void stopped_leading();

    // ---- Requests from followers -----------------------------------------------------------------------------------

    /// A publisher asks to follow the WAL. Only the leader serves it.
    void publisher_asks(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequestView& request, pubsub_itc_fw::Leading leading);
    /// A matching engine has asked afresh to be caught up: whatever it was sent as a follower of the WAL stops.
    void engine_asks_afresh(const pubsub_itc_fw::ConnectionID& connection);
    /// A matching engine asks to be caught up while this instance does not lead: it is declined, and told when this
    /// instance takes the lead.
    void engine_asks_while_not_leading(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequestView& request);
    /// Has the reading thread serve the engine on @p connection from the record after @p after_seq_no, up to the last
    /// order released.
    void serve_engine(const pubsub_itc_fw::ConnectionID& connection, int64_t after_seq_no, EngineServing serving);

    // ---- Sending to followers handed over ---------------------------------------------------------------------------

    /// Sends every publisher handed over each record committed through @p seq_no.
    void send_committed_to_publishers(int64_t seq_no);
    /// Sends each matching engine that follows the WAL, once handed over, every record through @p released_through.
    void send_released_to_follower_engines(int64_t released_through);

    // ---- Questions -------------------------------------------------------------------------------------------------

    /// Whether the reading thread is serving the follower on @p connection.
    [[nodiscard]] bool being_caught_up(const pubsub_itc_fw::ConnectionID& connection) const;
    /// Whether the engine on @p connection is being caught up because it asked to be.
    [[nodiscard]] bool caught_up_on_its_request(const pubsub_itc_fw::ConnectionID& connection) const;
    /// Whether the engine on @p connection follows the WAL, handed over.
    [[nodiscard]] bool follows_the_wal(const pubsub_itc_fw::ConnectionID& connection) const;

    // ---- PeerCatchUpInterface, for the replicated WAL ---------------------------------------------------------------

    void start_peer_catch_up(int64_t after_seq_no) override;
    void stop_peer_catch_up() override;
    [[nodiscard]] bool reading_has_stopped() const override;

    // ---- WalFollowerEventsInterface, for the reading thread's service -----------------------------------------------

    void follower_handed_over(const pubsub_itc_fw::ConnectionID& follower, int64_t through, int64_t records_sent,
                              pubsub_itc_fw::WalPosition resume_at) override;
    void hand_over_falling_behind(const pubsub_itc_fw::ConnectionID& follower) override;
    void follower_progress(const pubsub_itc_fw::ConnectionID& follower, int64_t sent_through) override;
    void follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no) override;
    void declined_while_not_leading(const pubsub_itc_fw::ConnectionID& follower) override;
    void reading_stopped() override;

  private:
    void engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent, pubsub_itc_fw::WalPosition resume_at,
                            const EngineServing& serving);
    void cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no, const std::string& which_follower);
    void handed_over_through(int64_t through);

    const Converters converters_;
    ServingWalFollowersLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    std::optional<pubsub_itc_fw::WalFollowerService> service_;
    std::optional<pubsub_itc_fw::WalHandedOverFollowers> handed_over_publishers_;
    // The matching engines that follow the WAL rather than lead, once caught up: each is sent every record as it is
    // released, as a publisher is sent each as it is committed (docs/availability/both_engines_apply_the_wal.md).
    std::optional<pubsub_itc_fw::WalHandedOverFollowers> handed_over_engines_;
    // The connection the follower sequencer is being sent its missing records on, while it is.
    pubsub_itc_fw::ConnectionID peer_catch_up_connection_;
    // Each matching engine the reading thread is serving, by connection.
    std::unordered_map<int, EngineServing> engine_servings_;

    // Each damaged record already said at Error, with the kind of follower it stopped; and the one standing now, 0 when
    // there is none.
    std::unordered_set<std::string> damaged_records_reported_;
    int64_t damaged_record_standing_{0};
};

} // namespaces
