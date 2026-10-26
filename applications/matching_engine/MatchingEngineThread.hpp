#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <optional>
#include <string>
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
#include <matching_engine_test_events.hpp>
#include <replication_messages.hpp>

#include "FixOrderLimits.hpp"
#include "GatewayIds.hpp"
#include "MatchingEngineConfiguration.hpp"
#include "MatchingEngineStateMachine.hpp"
#include "OrderBook.hpp"
#include "OrderBookMetricsReporter.hpp"
#include "OrderEntry.hpp"
#include "OrderKey.hpp"
#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/EpochStore.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>

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
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // Whether a command is applied live or as part of a catch-up, which is whether this instance is reconciling. For a
    // catch-up, also counts the record as offered to the state machine, which is how the catch-up is known complete.
    [[nodiscard]] CommandSource command_source_for(int64_t sequence_number);

    // Reusable scratch buffer for encoding an ExecutionReport before wrapping it in a
    // WalRecord envelope (send_er_to_sequencer). Grown to the largest ER seen and
    // reused -- no fixed cap that could silently drop an ER, no per-ER allocation.
    std::vector<uint8_t> er_encode_buffer_;

    // Wraps the ER in a WalRecord envelope and sends it to both sequencers. The identity of
    // the session whose order the report is about rides on the envelope, so the ER PDU itself
    // stays purely DD-derived. Every report carries it. The leading sequencer routes an
    // ordinary report by the echoed seq_no when it can, but a report with no originating
    // sequence (the seq_no==0 cancel-on-failover reports) has only the identity to go on, and
    // so does a report a sequencer kept while it was not leading and forwards after taking the
    // lead, because only the leader records which session sent which order
    // (docs/availability/change_of_sequencer_leader.md, section 4.4).
    //
    // What travels is the session's identity, never an address. The ME has no idea where a
    // session is connected -- and on the path that needs this most, promotion after a
    // gateway or ME failure, any address it remembered would name a process that has since
    // died. The sequencer holds the live bindings and resolves the identity when it sends.
    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no, const fix_common::SessionIdentity& session,
                              ReportIsRepeat repeat = ReportIsRepeat::no);

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
    // being passed down to it. Every report leaves through send_er_to_sequencer(), which the
    // state machine reaches from each of the places it produces a report, and most of those --
    // a report that a halt has cancelled an order, a report from a catch-up, a refusal -- have
    // no client order behind them at all. Carrying the time through the state machine to each
    // would be one chance per place to supply the wrong thing, and the wrong thing would be
    // recorded silently.
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

    // Where the state machine's results go: reports to the sequencers, changes to the book to the follower engine,
    // what became of each order to the test, and accepted orders to the counter of orders processed.
    class EngineOutputs : public MatchingEngineOutputsInterface {
      public:
        explicit EngineOutputs(MatchingEngineThread& owner) : owner_(owner) {}
        void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& session,
                         ReportIsRepeat repeat) override;
        void order_added_to_book(int64_t sequence_number, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                                 const OrderEntry& entry) override;
        void order_removed_from_book(int64_t sequence_number, const fix_common::SessionIdentity& session, std::string_view cl_ord_id) override;
        void trading_halt_changed(int64_t sequence_number, bool halted) override;
        void order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                           ReportIsRepeat repeat, int64_t sequence_number, std::string_view orig_cl_ord_id) override;
        void order_accepted() override;

      private:
        MatchingEngineThread& owner_;
    };
    EngineOutputs engine_outputs_{*this};

    // What makes the OrderID and ExecID values, from the secret in identifiers.secret_file. Read when the thread is
    // constructed, so that an engine whose secret cannot be read does not start.
    ReportIdentifiers report_identifiers_{ReportIdentifiers::read_secret_file(config_.identifier_secret_file)};

    // The handling of commands: what each new order and cancel does to the book, and the reports it produces. Declared
    // after the book, the maker of identifiers and the outputs it is given, so that they exist when it is constructed.
    MatchingEngineStateMachine state_machine_{order_book_, report_identifiers_, engine_outputs_, get_logger()};

    // The region this process inherited could not be read, so what it held is unknown here and
    // the book cannot be vouched for however well the reconciliation goes. R-0102.
    bool region_was_unusable_{false};

    // Helper: send a BookUpdate PDU to ME-secondary (primary only).
    void send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                          const OrderEntry* entry); // nullptr for Remove updates

    // Helper: apply a received BookUpdate PDU to the replica book (secondary only).
    void apply_book_update(const pubsub_itc_fw::EventMessage& message);

    // HA state machine (Slice C+D)

    // Current HA role. Unknown for a non-HA (single-instance) ME and for the
    // primary before it adopts leadership; Follower for the passive secondary.
    MeRole ha_role_state_{MeRole::Unknown};

    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already spent.
    int32_t epoch_{0};
    // How many reports this instance has sent to the sequencers while leading under epoch_, so that
    // each carries its position in the engine's reports (report_engine_epoch, report_number). Counted
    // from 1, and from 1 again under each new epoch.
    int64_t report_number_{0};
    int32_t report_number_epoch_{0};
    // Gives a report about to be sent its position in the reports this engine sends.
    void number_report(pubsub_itc_fw_app::WalRecord& envelope);

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    pubsub_itc_fw::EpochStore epoch_store_;

    // Where this instance's promises in deciding which instance leads outlive the process, until
    // the machine reboots. It lets a process restarted by its supervisor keep the lead it held.
    pubsub_itc_fw::LeasePromiseStore lease_promise_store_;
    // Writes promise records on a thread of its own, so that refreshing one does not stop this
    // instance answering lease requests while the disk is written (BUG-0107). Declared after the
    // store it writes to, so it is destroyed first. Constructed only when there is a lease agent.
    std::optional<pubsub_itc_fw::BackgroundPromiseRecorder> background_promise_recorder_;

    // Drives the lease rules that decide whether this instance leads. Recurring.
    pubsub_itc_fw::TimerID lease_tick_timer_id_{};

    // How the lease rules reach the other two voters: the peer instance over whichever replication
    // connection is up, and the arbiter pool on both arbiter connections.
    class EngineLeaseLinks : public pubsub_itc_fw::LeaseLinksInterface {
      public:
        explicit EngineLeaseLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) override;
        void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) override;

      private:
        MatchingEngineThread& owner_;
    };
    EngineLeaseLinks lease_links_{*this};

    // Decides whether this instance leads: it does only while a majority of three voters -- itself,
    // its peer and the arbiter pool -- has granted it a lease that has not run out. Constructed when
    // the application is ready, if high availability is on. See pubsub_itc_fw/PairLeaseAgent.hpp.
    std::optional<pubsub_itc_fw::PairLeaseAgent> lease_agent_;

    // The test events this thread sends, when a test started this instance (docs/availability/test_events.md).
    class EngineTestEventLinks : public pubsub_itc_fw::TestEventLinksInterface {
      public:
        explicit EngineTestEventLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override;

      private:
        MatchingEngineThread& owner_;
    };
    EngineTestEventLinks test_event_links_{*this};
    std::optional<pubsub_itc_fw::TestEventChannel> test_events_;

    // Reports a change of role to the test, if one started this instance.
    void report_role_change(MeRole from, MeRole to);

    // Reports what the engine did with an order or a cancel to the test, if one started this instance and listed the
    // session's comp id in report_orders_of. With no test, this costs one test of whether the channel exists.
    // Reports a stage of catching up from the sequencer's WAL to the test, if one started this instance.
    void report_catch_up(pubsub_itc_fw_app::CatchUpStage stage, int64_t through_seq_no, int64_t records_sent);

    // Reports where this instance stands, when the connection to the test is made, so that what happened before then is
    // not lost: its region found unusable at startup; trading halted; and catching up, or caught up, and if caught up at
    // startup, waiting to be told what it may do.
    void report_present_state();

    // What the sequencer said it had sent, in the catch-up that last completed, for report_present_state.
    int64_t last_catch_up_records_sent_{0};

    // Why the region holding the open orders could not be used at startup, or empty if it could; and why trading is
    // halted, once it is. Kept for the test, which may connect after either happened (report_present_state).
    std::string region_unusable_reason_;
    std::optional<pubsub_itc_fw_app::HaltReason> halt_reason_;

    // While this instance waits for the TechnicalEvent halting trading that it asked for, the timer that asks again.
    bool halt_awaited_{false};
    pubsub_itc_fw::TimerID halt_request_timer_id_{};
    static constexpr std::chrono::seconds halt_request_interval{1};

    void report_order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                              ReportIsRepeat repeat, int64_t sequence_number, std::string_view orig_cl_ord_id = {});

    // Armed while this instance is reconciling, and cancelled by the ack. Only a sequencer
    // that leads serves a catch-up; one that is not leading drops the request without
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
        /// The number the report was given when first sent, kept so that each sequencer sees the same.
        bool numbered{false};
        int32_t report_engine_epoch{0};
        int64_t report_number{0};
        /// Which connections the report has been sent on; it is kept until it has been sent on each.
        bool sent_to_primary{false};
        bool sent_to_secondary{false};
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
    // Whether reports are being held because there is no connection to a sequencer to send them on
    // (docs/bug_list.md, BUG-0120). Logged when it starts, and cleared when they are sent.
    bool waiting_for_report_connection_{false};
    // Sends the reports held when a connection for reports is established, unless this instance is
    // still waiting to be told whether it may serve.
    void release_reports_held_for_a_connection();

    [[nodiscard]] bool holding_reports_until_entitled() const;
    void release_held_reports();
    // Sends each held report on each of the named connections it has not yet been sent on, and drops
    // those sent on every connection there is.
    void send_held_reports(bool to_primary, bool to_secondary);
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
    // lease to trigger one, so it reconciles when the sequencer first connects -- once,
    // not on every reconnect: this instance's position advances as it works and is not tracked
    // between catch-ups, so asking again later would replay what it has already applied.
    bool has_reconciled_{false};

    // Whether the current catch-up has been asked for on at least one sequencer order connection. False from the moment
    // a catch-up begins until the request is first sent, which is later when no sequencer had connected yet.
    bool follow_request_sent_{false};

    // Whether this instance knows where it had got to. A process that found a region left by a
    // predecessor does: the region says which sequence number its contents are current to. One
    // that found no region at all does not, and must not pretend the answer is zero -- asking
    // for everything after zero asks for the whole retained log, which is every order the venue
    // has taken since the last reclaim rather than the few it missed.
    bool has_position_{false};

    // Whether this catch-up is part of taking the role, or only part of becoming current.
    // Being current and being entitled to act are separate things, and only the lease rules say
    // who acts. A catch-up done at startup ends with the instance current and still waiting to
    // be told; one begun because this instance is to lead ends with it leading.
    bool reconciling_to_lead_{false};

    // That this instance granted its peer a lease while it was still catching up at startup, and
    // so agreed that the peer leads. It cannot follow until the catch-up ends, so this is acted on
    // then.
    //
    // The epoch is re-checked when it is acted on rather than when it is stored, because
    // reconciliation can advance this instance's own epoch in between.
    bool peer_leader_heard_while_reconciling_{false};
    int64_t peer_leader_instance_id_{0};
    int32_t peer_leader_epoch_{0};

    pubsub_itc_fw::TimerID book_metrics_timer_id_{};

    // The pair's fixed identities. Neither ever changes for the life of a deployment. The only
    // preference between them is that the secondary waits longer before it first asks to lead, so
    // the primary leads when both start together; see pubsub_itc_fw/PairLeaseAgent.hpp.
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
    // more than one. On promotion we send WalFollowRequest to every one of them:
    // the ME cannot tell which sequencer is the leader, so it asks all and lets the
    // leader serve WAL catch-up while followers re-point without streaming (see
    // SequencerThread::handle_me_position_request). This guarantees the leader --
    // whichever it is -- reconciles this ME and re-routes live orders here.
    std::unordered_set<pubsub_itc_fw::ConnectionID> sequencer_order_conn_ids_;

    // Arbiter-mediated promotion helpers.
    void enter_follower_state();
    void adopt_leader_role();
    /// Changes what this instance does to follow what the lease rules have just decided.
    void act_on(pubsub_itc_fw::PairLeaseAgent::Change change);
    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_grant(const pubsub_itc_fw::EventMessage& message);
    void handle_lease_refusal(const pubsub_itc_fw::EventMessage& message);

    // WAL reconciliation (RECONCILING state).

    /**
     * @brief Tells the sequencers which role this instance now holds, and under which epoch.
     *
     * The sequencer used to decide where to send orders by which socket had connected, which
     * was correct only while primary and leader meant the same thing. Announcing the role
     * explicitly is what lets it route to whoever leads. See docs/availability/design_notes.md#ha_arbiter_only_arbitrates.
     */
    void announce_role();

    /// The role as a word, for log lines that a person will read after an incident.
    [[nodiscard]] static const char* me_role_name(MeRole role);
    void set_epoch(int32_t new_epoch);
    void publish_book_metrics();

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

    void begin_reconciliation();

    /// Take the leader role, catching up from the WAL first unless this instance already leads.
    /// A follower has been passive and its replica may be behind. An instance that caught up
    /// when the first sequencer connected, and has waited since to learn its role, has been sent
    /// no orders meanwhile, because a sequencer sends orders only to the engine that leads. An
    /// instance that already leads has applied every record as it arrived, and must not catch
    /// up, because a catch-up makes live orders arriving meanwhile read as replay: applied
    /// silently, with no acceptance reported to the member.
    void become_leader_when_current();
    // Asks every sequencer to be caught up (WalFollowRequest); only the leading one serves it.
    void send_follow_request();
    // A sequencer order connection was made while this instance is catching up. Asks on that connection alone if the
    // catch-up has already been asked for on others, so that a stream already under way is left to finish (BUG-0200);
    // otherwise begins the catch-up.
    void ask_on_new_sequencer_connection(const pubsub_itc_fw::ConnectionID& id);
    // The catch-up request this instance sends, naming it follower_name, which must outlive the request.
    [[nodiscard]] pubsub_itc_fw::WalFollowRequest follow_request(const std::string& follower_name) const;
    // The leading sequencer has sent every order after this instance's position (WalFollowCaughtUp).
    void handle_caught_up(const pubsub_itc_fw::EventMessage& message);
    // A sequencer that does not lead has declined the request (WalFollowDeclined); it says when it leads.
    void handle_follow_declined(const pubsub_itc_fw::EventMessage& message);
    // A sequencer has taken the lead (WalLeaderChanged): a request still unanswered is made again at once.
    void handle_leader_changed(const pubsub_itc_fw::EventMessage& message);
    // A sequencer that has just taken the lead asks for the highest order or cancel this engine has
    // acted on, so that it can send any its log holds above that (EnginePositionQuery, 118).
    void handle_engine_position_query(const pubsub_itc_fw::EventMessage& message);

    // The settings that change what the engine does with a command, which the two engines of a pair must share
    // (docs/availability/deterministic_matching_engine.md, section 5). Sent to the peer when this instance's
    // connection to the peer's replication listener is established.
    void send_engine_settings();
    // The peer's settings arrived: compared with this instance's, and acted on by bar_peer_if_settings_differ.
    void handle_engine_settings(const pubsub_itc_fw::EventMessage& message);
    // While this instance leads, says that its peer may not lead if the peer's settings differ from its own, and that
    // it may lead again if they no longer differ. Called when the peer's settings arrive and when this instance takes
    // the lead. Returns whether it has just said its peer may not lead, which it reports to the test.
    [[nodiscard]] bool bar_peer_if_settings_differ();
    // Tells the test, if one started this instance, that the peer's settings differ, and whether this instance, leading,
    // has said its peer may not lead.
    void report_settings_differ_from_peer();

    // Asks every sequencer for a CancelBook command through cancel_book_awaited_through_; the leading one writes it.
    void request_cancel_book();
    // Applies a CancelBook command, and, where it is the one this promoted instance asked for, says that it has resumed.
    void apply_cancel_book_command(const pubsub_itc_fw_app::CancelBookView& command, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                                   CommandSource source);
    // Applies a TechnicalEvent command, and tells the test of a halt it set.
    void apply_technical_event_command(const pubsub_itc_fw_app::TechnicalEventView& event, int64_t sequence_number, int32_t trading_date, CommandSource source);
    // Tells the test that this instance resumed as leader, and with how many orders.
    void report_resumed_as_leader();

    // While a promoted instance waits to apply the command cancelling what it inherited: the position it asked to cancel
    // through, which is the position it caught up to. Zero when it waits for none. Asked for again every
    // cancel_book_request_interval, because a request sent as the sequencers change leader may reach no leader.
    int64_t cancel_book_awaited_through_{0};
    std::chrono::steady_clock::time_point cancel_book_requested_at_{};
    static constexpr std::chrono::seconds cancel_book_request_interval{1};
    // The position the last catch-up before leading reached, which the test is told when this instance resumes.
    int64_t resumed_through_{0};

    // The peer's open-order region capacity and the fingerprint of its identifier secret, as it last said, and whether
    // either differs from this instance's. Kept when the connection to the peer is lost: a peer that restarts says again
    // what it is configured with.
    bool peer_settings_known_{false};
    int64_t peer_region_capacity_{0};
    int64_t peer_secret_fingerprint_{0};
    bool peer_settings_differ_{false};

    /**
     * @brief Whether the engine can say what it is holding.
     *
     * The region vouches for the book. Where the one this process inherited could not be used, the
     * engine asks for every record from the first, and holds what that replay rebuilt: the venue's
     * book, if the WAL reached the first command of the trading day, and otherwise an incomplete one
     * (R-0102, R-0123). See account_for_unusable_region.
     */
    [[nodiscard]] bool book_can_be_vouched_for() const;

    /**
     * @brief Decides, once a catch-up has finished, what an engine whose region could not be used does with the book the
     * catch-up rebuilt (R-0123).
     *
     * Where the WAL reached the first command of the trading day, the book is exactly the venue's, so the engine carries
     * on with it, cancelling nothing, and reports at Error that it rebuilt its record. Where it did not, the engine cannot
     * account for the orders the venue holds: if it is about to lead, it asks the leading sequencer to halt trading, by a
     * TechnicalEvent in the WAL, so that every engine halts at the same point; if it follows, the engine that leads goes
     * on trading, and this one asks again when it is to lead.
     */
    void account_for_unusable_region(const pubsub_itc_fw::WalFollowCaughtUpView& ack);

    // Asks every sequencer for a TechnicalEvent halting trading because this instance cannot account for the book; the
    // leading one writes it. Asked again every halt_request_interval until trading is halted.
    void request_halt();
};

} // namespaces
