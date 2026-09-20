#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/CatchUpTally.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/CounterHandle.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/IncrementalRehashMap.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <matching_engine_replication.hpp>

#include "EpochStore.hpp"
#include "FixOrderLimits.hpp"
#include "GatewayIds.hpp"
#include "MatchingEngineConfiguration.hpp"
#include "OrderBook.hpp"
#include "OrderBookMetricsReporter.hpp"
#include "OrderEntry.hpp"
#include "OrderKey.hpp"

namespace matching_engine {

/**
 * @brief HA state for the matching-engine instance (Slice C+D).
 *
 * Unknown    -- HA disabled or not yet classified.
 * Follower   -- passive replica: receives BookUpdate PDUs, discards order and ER PDUs.
 * Reconciling-- promotion in progress: replaying WAL catch-up from the sequencer,
 *               applying NOS/OCR to the book WITHOUT emitting ERs.
 * Leader     -- active: processes orders and emits ERs normally.
 */
enum class MeRole { Unknown, Follower, Reconciling, Leader };

/**
 * @brief ApplicationThread subclass implementing the matching engine stub.
 *
 * Receives sequenced order PDUs from the sequencer on the inbound listener,
 * maintains a primitive order book keyed by ClOrdID, and sends ExecutionReport
 * PDUs back to the sequencer over the outbound `sequencer_er` connections.
 * The sequencer routes ERs to the originating gateway.
 *
 * Order lifecycle:
 *   NOS (new ClOrdID)      -> ER ExecType=New  / OrdStatus=New (order enters book)
 *   NOS (duplicate ClOrdID)-> ER ExecType=Rejected / OrdRejReason=DuplicateOrder
 *   OCR (known OrigClOrdID)-> ER ExecType=Canceled / OrdStatus=Canceled (removed from book)
 *   OCR (unknown OrigClOrdID) -> ER ExecType=Rejected / OrdRejReason=UnknownOrder
 *
 * There is no real matching logic; orders sit as New until cancelled.
 *
 * Threading: ThreadID 1.
 */
class MatchingEngineThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger. Must outlive this object.
     * @param[in] reactor  Owning Reactor. Must outlive this object.
     * @param[in] config   Matching engine configuration.
     */
    MatchingEngineThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                         const MatchingEngineConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // Helper: format "ME-ORD-N" or "ME-EXEC-N" into a caller-provided stack buffer.
    // Returns string_view into that buffer. Buffer must outlive the view.
    template <size_t N> static std::string_view format_id(std::array<char, N>& buf, const char* prefix, size_t prefix_len, int64_t counter) {
        std::memcpy(buf.data(), prefix, prefix_len);
        auto [end, ec] = std::to_chars(buf.data() + prefix_len, buf.data() + N, counter);
        return {buf.data(), static_cast<size_t>(end - buf.data())};
    }

    // Both come from the WalRecord envelope, not the (DD-derived) FIX PDU:
    // sequenced_at_ns is the sequencer's wall time used as transact_time (0 => not
    // stamped, fall back to the local wall clock); session is the identity of the client
    // session that placed the order -- its comp id and protocol -- which forms half of the
    // order key. Deliberately not the connection it arrived on: that is where the member
    // was, and the book has to be keyed on who it is, or an order becomes unmanageable the
    // moment its connection drops. An empty identity means a record with no originating
    // client session at all.
    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no, int64_t sequenced_at_ns,
                                 const fix_common::SessionIdentity& session);
    void handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t seq_no, int64_t sequenced_at_ns,
                                     const fix_common::SessionIdentity& session);
    // Reusable scratch buffer for encoding an ExecutionReport before wrapping it in a
    // WalRecord envelope (send_er_to_sequencer). Grown to the largest ER seen and
    // reused -- no fixed cap that could silently drop an ER, no per-ER allocation.
    std::vector<uint8_t> er_encode_buffer_;

    // Reusable arena backing the ER's echoed group element arrays (handle_new_order_single).
    // Sized to need (grow-and-retry), not a fixed cap that could silently drop echoed groups.
    static constexpr size_t initial_er_group_arena_size = 4096;
    static constexpr size_t max_er_group_arena_size = 1u << 20; // 1 MiB sanity ceiling
    std::vector<uint8_t> er_group_arena_buffer_ = std::vector<uint8_t>(initial_er_group_arena_size);

    // Wraps the ER in a WalRecord envelope and sends it to the sequencer(s). Routing
    // metadata for ERs not tied to a sequenced order (the seq_no==0 cancel-on-failover
    // ERs) rides on the envelope, so the ER PDU itself stays purely DD-derived. For
    // ordinary ERs the sequencer routes by the echoed seq_no, so no session is supplied.
    //
    // What travels is the session's identity, never an address. The ME has no idea where a
    // session is connected -- and on the path that needs this most, promotion after a
    // gateway or ME failure, any address it remembered would name a process that has since
    // died. The sequencer holds the live bindings and resolves the identity when it sends.
    /// Whether a report repeats one whose subject the member may already have been told
    /// about. Carried to the gateway on the envelope and written there as PossResend
    /// (tag 97). See R-0122.
    enum class ReportIsRepeat { no, yes };

    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no,
                              const fix_common::SessionIdentity& session = fix_common::SessionIdentity{}, ReportIsRepeat repeat = ReportIsRepeat::no);

    const MatchingEngineConfiguration& config_;

    /**
     * Orders accepted onto the book, incremented on the one path that puts one there.
     *
     * Deliberately not a count of order PDUs arriving. The reconciliation path replays the
     * WAL into the book without acknowledging anything and the follower path discards
     * orders outright, so counting arrivals would make the series jump by the whole
     * replayed backlog at each failover and stop meaning "orders the venue processed".
     * Duplicate ClOrdIDs, which are rejected, are not counted either.
     *
     * Cancels are not counted here. A cancel is a different operation on an existing
     * order, and folding the two together would leave neither rate readable.
     */
    pubsub_itc_fw::CounterHandle orders_processed_counter_;

    // HA role flags (set from configuration at construction).
    bool ha_enabled_{false};
    bool is_primary_{true};

    // Whether a promotion cancels the book it inherits, from order_book.open_orders_on_promotion.
    // The policy is stated in configuration rather than settled here because keeping the book is
    // only safe once the catch-up behind it is verified; see the configuration field's comment.
    bool cancel_open_orders_on_promotion_{true};

    // Primary: outbound connection to ME-secondary's book replication listener.
    pubsub_itc_fw::ConnectionID outbound_replication_conn_id_;

    // Secondary: inbound replication connection from ME-primary.
    // Any inbound connection on the secondary IS the replication channel
    // (the secondary only has one inbound listener: the replication port).
    pubsub_itc_fw::ConnectionID inbound_replication_conn_id_;

    // Secondary: seq_no of the most recently applied BookUpdate.
    // Carried forward to WAL reconciliation at promotion time (Slice C).
    int64_t last_replicated_seq_no_{0};

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Order book keyed by (session identity, cl_ord_id).
    // Primary:   live orders currently on the book.
    // Secondary: replica of the primary's book, maintained via BookUpdate PDUs.
    //
    // The book reports the growth of its map directly to book_growth_reporter_ rather than
    // through an allocator: the map owns its tables outright and knows each one's size at the
    // point it allocates it, so the reporter is told the truth without an allocator in between.
    // The instrumentation matters because the framework's pool and slab allocators cover
    // objects with a message lifetime, and the book is long-lived state that grows --
    // uninstrumented, it reached 9.9 GB and the process was OOM-killed having logged no memory
    // warning. The orders themselves are in a mapped region of a fixed size, which cannot grow.
    pubsub_itc_fw::AllocationGrowthReporter book_growth_reporter_;

    // How big the book actually is. Sampled on a timer rather than written on every
    // order: the value is wanted as a trend over a trading day, and touching a gauge
    // on the order path would put metrics work in the hot path to buy resolution
    // nobody reads.
    fix_common::OrderBookMetricsReporter book_metrics_;

    // How long the order had already been inside the venue when it arrived here, and again
    // when its acknowledgement was sent back. The difference between the two is what the
    // matching engine costs an order. See OrderPathMetrics.hpp for how the measurement works.
    pubsub_itc_fw::HistogramHandle order_in_elapsed_histogram_;
    pubsub_itc_fw::HistogramHandle er_out_elapsed_histogram_;

    // The time the gateway first read the order that is being handled right now, and whether
    // there is one. Both are cleared at the start of every inbound message and set again only
    // where a client order is unwrapped, so outside the handling of an order there is nothing
    // here to read.
    //
    // This exists so that the departure of an acknowledgement can be timed without the value
    // being passed down to it. Reports leave through send_er_to_sequencer(), which is called
    // from ten places, and most of them -- a report that a halt has cancelled an order, a
    // report reconciled after a failover, a fill of an order that has rested for hours -- have
    // no client order behind them at all. Adding a parameter to all ten would be one chance
    // per call site to supply the wrong thing, and the wrong thing would be recorded silently.
    //
    // What makes reading it correct is that only an acknowledgement whose status is New is
    // ever timed. Such an acknowledgement can only arise from handling a new order, which is
    // the very order whose time is remembered here. Every other kind of report carries some
    // other status and never reads these at all.
    bool has_current_order_ingress_ns_{false};
    int64_t current_order_ingress_ns_{0};

    // Scope for the book's gauges. A metric key token, so [A-Za-z0-9_]+ only.
    static constexpr const char* book_metrics_scope = "order_book";

    // Often enough to show a burst of resting orders building, rare enough to be
    // invisible against order flow. Matches the gateways' pool sampling.
    static constexpr std::chrono::seconds book_metrics_sample_interval{5};

    // The orders the venue is holding, in a region that outlives this process.
    // See docs/durability/open_order_checkpoint.md.
    OrderBook order_book_;

    // The region this process inherited could not be read, so what it held is unknown here and
    // the book cannot be vouched for however well the reconciliation goes. R-0102.
    bool region_was_unusable_{false};

    // How long the venue was unable to match, worked out at startup from the region and kept
    // until there is somewhere to send the reports. Zero where there was no previous life.
    int64_t absence_ns_{0};

    // Set at startup when the absence was too long and orders were open. Acted on at the point
    // the engine would otherwise resume trading, because cancelling means telling each member,
    // and at startup there is no connection to tell anyone down.
    bool cancel_and_halt_pending_{false};

    // Trading is halted and the venue is not taking orders. R-0117 halts here after a long
    // absence; R-0023 requires a person to lift it.
    bool halted_{false};

    // Says the engine is alive and able to match, independently of whether anything is being
    // traded. A second is far finer than the absence limit needs and costs one store to
    // mapped memory.
    pubsub_itc_fw::TimerID able_to_match_timer_id_{};
    static constexpr std::chrono::seconds able_to_match_interval{1};

    // Monotonic counters for generated OrderID and ExecID values (primary only).
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};

    // Helper: send a BookUpdate PDU to ME-secondary (primary only).
    void send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                          const OrderEntry* entry); // nullptr for Remove updates

    // Helper: apply a received BookUpdate PDU to the replica book (secondary only).
    void apply_book_update(const pubsub_itc_fw::EventMessage& message);

    // HA state machine (Slice C+D)

    // Current HA role. Unknown for a non-HA (single-instance) ME and for the
    // primary before it adopts leadership; Follower for the passive secondary.
    MeRole ha_role_state_{MeRole::Unknown};

    // Secondary: true while the promotion-timeout timer is armed (primary
    // replication connection has been lost and we are waiting to see if it
    // reconnects before requesting arbitration).
    bool promotion_pending_{false};

    // Leadership generation. Adopted from ArbitrationDecision, or self-incremented
    // on degraded self-promotion.
    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already spent.
    int32_t epoch_{0};

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    fix_common::EpochStore epoch_store_;

    // Timer ids (default-constructed = not scheduled); on_timer_event compares
    // a fired timer's id against these to identify it.
    pubsub_itc_fw::TimerID promotion_timeout_timer_id_{};
    pubsub_itc_fw::TimerID arbiter_heartbeat_timer_id_{};

    // Armed when a primary asks the arbiter who leads at startup, and cancelled by the
    // answer. If it fires, no arbiter replied and the venue would otherwise have no matching
    // engine leader at all, so the instance-id rule is applied locally and logged as degraded
    // -- the same fallback the sequencer has, and for the same reason.
    pubsub_itc_fw::TimerID startup_arbitration_timer_id_{};

    // Armed while this instance is reconciling, and cancelled by the ack. Only a sequencer
    // that leads serves a catch-up; one that is still electing drops the request without
    // answering. So the request is re-sent while the answer is outstanding, rather than
    // assumed to have been received -- an instance that asked once at the wrong moment would
    // wait for an answer nobody is going to send.
    pubsub_itc_fw::TimerID reconciliation_timer_id_{};
    int reconciliation_attempts_{0};

    // Records applied since reconciliation began, and the figure at the previous tick. A
    // request is only repeated when nothing has arrived since the last one: a sequencer that
    // is serving the catch-up is sending records, and asking it again mid-stream would have it
    // send the whole catch-up a second time -- which arrives after this instance has promoted
    // and is then processed as live traffic, duplicating every order in it.
    int64_t reconciliation_records_seen_{0};
    int64_t reconciliation_records_at_last_tick_{0};

    // Reports sent out of the catch-up, counted so the engine can say what it did. An operator
    // reading this line learns how much a member was told on a promotion, which is otherwise
    // visible only at Debug and only per record.
    int64_t reconciliation_reports_sent_{0};

    // A report produced by a catch-up done at startup, held until this instance knows whether it
    // is going to serve anybody.
    //
    // The engine catches up before it has been told what it may do -- that is the whole point of
    // doing it at the first sequencer connection -- so an instance that is about to learn its peer
    // leads would otherwise report every recovered order to the member that placed it and then
    // serve nobody. Reporting is an act, and BUG-0064's fifth trap says being current and being
    // entitled to act are separate things. So the reports wait: released if this instance adopts
    // the leader role, discarded if it becomes a follower, in which case the leader has already
    // answered those members.
    //
    // The encoded bytes are held rather than the ExecutionReport, whose string fields point into
    // the decode arena and do not outlive the record that produced them.
    struct HeldReport {
        std::vector<uint8_t> payload; ///< the encoded ExecutionReport
        int64_t seq_no{0};            ///< echoed in the transport header, as a live report's is
        fix_common::SessionIdentity session;
        bool poss_resend{false};
    };
    std::vector<HeldReport> held_reports_;

    // What a held report costs is its encoded size, a few hundred bytes, and a catch-up is
    // bounded by the venue's retained log rather than by anything this instance controls. Past
    // this many the reports are sent as they were before, and the reason is logged once: a
    // member that already holds the report can discard a duplicate, and one that never receives
    // it cannot recover from the absence. Answering is worth more than tidiness, which is the
    // same ordering BUG-0009 settled.
    static constexpr size_t max_held_reports_ = 50000;
    bool held_reports_overflowed_{false};

    [[nodiscard]] bool holding_reports_until_entitled() const;
    void release_held_reports();
    void discard_held_reports(const char* reason);

    // Establishes that the catch-up was complete before this instance acts on it (R-0101).
    //
    // Its life is one ASK rather than one reconciliation, unlike the counter above: the answer to
    // a repeated request is the whole stream again, so what the ack accounts for is what the last
    // request produced. It is reset where the request is sent.
    pubsub_itc_fw::CatchUpTally catch_up_tally_{0};

    // Whether this catch-up is a promotion or a start. Both catch up, because both may be
    // missing records; only a promotion inherits a book from a peer, and only a promotion is
    // what order_book.open_orders_on_promotion speaks about. A restart that recovered its own
    // region is R-0018's case and keeps what it recovered whatever that setting says.
    bool reconciling_from_follower_{false};

    // Whether the catch-up has been done. A venue with high availability off has no
    // arbitration to trigger one, so it reconciles when the sequencer first connects -- once,
    // not on every reconnect: this instance's position advances as it works and is not tracked
    // between catch-ups, so asking again later would replay what it has already applied.
    bool has_reconciled_{false};

    // Whether this instance knows where it had got to. A process that found a region left by a
    // predecessor does: the region says which sequence number its contents are current to. One
    // that found no region at all does not, and must not pretend the answer is zero -- asking
    // for everything after zero asks for the whole retained log, which is every order the venue
    // has taken since the last reclaim rather than the few it missed.
    bool has_position_{false};

    // Whether this catch-up is part of taking the role, or only part of becoming current.
    // Being current and being entitled to act are separate things, and only the arbiter says
    // who acts. A catch-up done at startup ends with the instance current and still waiting to
    // be told; one begun because this instance is to lead ends with it leading.
    bool reconciling_to_lead_{false};

    // A peer's claim to lead, heard while this instance was still reconciling and therefore not
    // yet in a state to act on it. Kept rather than discarded: the leader announces when a
    // replication link comes up, which is the one announcement a restarting instance is offered,
    // and a healthy leader has no reason to send another. Dropping it left this instance with
    // nothing telling it a leader existed, so the startup arbitration deadline degraded it into a
    // second one -- BUG-0082's second half, measured by ha_test.py scenario 57.
    //
    // The epoch is re-checked when it is acted on rather than when it is stored, because
    // reconciliation can advance this instance's own epoch in between.
    bool peer_leader_heard_while_reconciling_{false};
    int64_t peer_leader_instance_id_{0};
    int32_t peer_leader_epoch_{0};

    pubsub_itc_fw::TimerID book_metrics_timer_id_{};

    /// How many times a starting instance asks the arbiter before giving up and degrading.
    /// More than one because an arbiter that has itself restarted declines to answer until it
    /// knows who leads, and that silence is a reason to wait rather than to promote.
    static constexpr int max_startup_arbitration_attempts = 3;
    int startup_arbitration_attempts_{0};

    // Secondary: instance_id of the primary (peer). Fixed at 1 by convention.
    // The pair's fixed identities. Primary is always the lower id -- the arbiter's cold-start
    // preference relies on it -- and neither ever changes for the life of a deployment.
    // Which of them LEADS is a separate question and moves; see docs/availability/design_notes.md#ha_restart_role.
    static constexpr int64_t primary_instance_id = 1;
    static constexpr int64_t secondary_instance_id = 2;

    /// The other instance of this pair. Was hard-coded to the primary back when only the
    /// secondary ever asked for arbitration; now that a restarting primary asks too, an
    /// instance that named the primary unconditionally would report itself as its own peer.
    [[nodiscard]] int64_t peer_instance_id() const {
        return config_.instance_id == primary_instance_id ? secondary_instance_id : primary_instance_id;
    }

    // Outbound connections to the arbiter pool (both roles when HA is enabled).
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // Inbound order connections from the sequencers. Both the leader and follower
    // sequencer open a (pre-warmed) order connection to this ME, so there may be
    // more than one. On promotion we send MePositionRequest to every one of them:
    // the ME cannot tell which sequencer is the leader, so it asks all and lets the
    // leader serve WAL catch-up while followers re-point without streaming (see
    // SequencerThread::handle_me_position_request). This guarantees the leader --
    // whichever it is -- reconciles this ME and re-routes live orders here.
    std::unordered_set<pubsub_itc_fw::ConnectionID> sequencer_order_conn_ids_;

    // Arbiter-mediated promotion helpers.
    void enter_follower_state();
    void adopt_leader_role();
    void send_arbitration_report();
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);
    void send_arbiter_heartbeat();

    // WAL reconciliation (RECONCILING state).

    /**
     * @brief Asks the arbiter which instance leads, rather than assuming it is this one.
     *
     * A primary used to adopt LEADER the moment its first arbiter connection came up. That is
     * harmless on a cold start, where the arbiter would name it anyway, and wrong on a restart:
     * the peer may already have been promoted and be serving, and the venue ends up with two
     * leaders. See docs/bug_list.md, BUG-0042.
     */
    void request_startup_arbitration();

    /**
     * @brief Tells the sequencers which role this instance now holds, and under which epoch.
     *
     * The sequencer used to decide where to send orders by which socket had connected, which
     * was correct only while primary and leader meant the same thing. Announcing the role
     * explicitly is what lets it route to whoever leads. See docs/availability/design_notes.md#ha_arbiter_only_arbitrates.
     */
    void announce_role();

    /**
     * @brief Starts the recurring message to the arbiters, in whatever role this instance holds.
     *
     * Previously started only on becoming leader, which meant a follower never reached the
     * arbiter at all -- and the arbiter registers a component when it hears from it, so it had
     * no way of knowing a follower was connected. Its cold-start rule asks exactly that.
     */
    void start_arbiter_heartbeats();

    /// The role as a word, for log lines that a person will read after an incident.
    [[nodiscard]] static const char* me_role_name(MeRole role);
    void set_epoch(int32_t new_epoch);
    void publish_book_metrics();
    void handle_peer_role_announcement(const pubsub_itc_fw::EventMessage& message);

    /// Defer to a peer that says it leads, taking its generation rather than inventing one.
    /// Returns false where the claim is refused, which is only ever because it is stale.
    bool follow_peer_claiming_leadership(int64_t instance_id, int32_t announced_epoch);

    /// Act on a leader announcement that arrived while this instance was reconciling, now that
    /// it has somewhere to act from. Consumed either way, so a claim is never applied twice.
    void act_on_peer_leader_heard_while_reconciling();

    /**
     * @brief Reads the open orders back out of the region, if a previous process left any.
     * @param[in] region_existed Whether open() found a region rather than creating one.
     *
     * Files every record the region vouches for, takes the position it was current to as the
     * point reconciliation resumes from, and carries the order numbering forward so a
     * successor does not reissue one. Where there was nothing to recover it touches the
     * region instead, so that no order pays a page fault (R-0121).
     */
    void recover_open_orders(bool region_existed);

    /**
     * @brief Whether the venue was unable to match for longer than it says it may be.
     *
     * Compares the wall-clock time the previous owner of the region last said it was working
     * against the time this one started. That is the whole absence -- noticing the death,
     * restarting the process, and rebuilding the book are all terms in it -- and the engine
     * works it out alone, because nothing here may depend on a supervisor being present.
     *
     * False where there was no region, because there was no previous life to be absent from.
     */
    [[nodiscard]] bool absence_was_too_long() const;

    /// Says the engine is able to match. On a timer, never from the order path: see R-0118.
    void mark_able_to_match();

    void begin_reconciliation();

    /// Take the leader role, catching up first where this instance is not already current.
    /// A follower has been passive and its replica may be behind; an instance that has not yet
    /// reconciled at all has never asked what it missed. One that has been serving since it
    /// started is current by definition -- it has applied every record as it arrived -- and
    /// must not reconcile, because a catch-up makes live orders arriving meanwhile read as
    /// replay: applied silently, with no acceptance reported to the member.
    void become_leader_when_current();
    void send_me_position_request();
    void handle_me_position_ack(const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief Whether the engine can say what it is holding.
     *
     * The region vouches for the book. Where it cannot be used the engine holds only what the
     * sequencer's tail replayed, which is not the same thing, and the answer is to cancel
     * everything, report each cancel, and halt -- R-0102 and R-0123.
     */
    [[nodiscard]] bool book_can_be_vouched_for() const;

    void cancel_all_orders_on_failover();

    /// Does the cancel-and-halt the absence rule asked for, once there is somewhere to send
    /// the reports. A no-op when none was asked for, so it is safe to call from every point
    /// the engine might reach first.
    void act_on_pending_halt();

    /**
     * @brief Cancels every open order, tells each member, and halts trading. R-0117.
     *
     * The orders are not stale because they are old. They are stale because the members that
     * placed them were locked out of them while the market moved, and could not act.
     */
    void cancel_everything_and_halt(const char* why);
};

} // namespaces
