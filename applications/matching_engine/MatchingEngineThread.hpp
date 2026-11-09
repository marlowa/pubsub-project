#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <deque>
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

#include "DiedApplyingCheck.hpp"
#include "EngineCatchUp.hpp"
#include "EngineReportPacing.hpp"
#include "EngineReportSending.hpp"
#include "EngineRole.hpp"
#include "FixOrderLimits.hpp"
#include "FollowerReportComparison.hpp"
#include "GatewayIds.hpp"
#include "HeldEngineReport.hpp"
#include "MatchingEngineConfiguration.hpp"
#include "MatchingEngineStateMachine.hpp"
#include "OrderBook.hpp"
#include "OrderBookMetricsReporter.hpp"
#include "OrderEntry.hpp"
#include "OrderKey.hpp"
#include "PeerSettingsCheck.hpp"
#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/EpochRecording.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>

namespace matching_engine {

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
    void on_connection_writable(pubsub_itc_fw::ConnectionID id) override;
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

    // The connection this instance made to its peer's listener ([peer]), and the one its peer made to this instance's.
    // Whichever is up carries the lease requests and grants between the pair, the role announcements and the settings
    // check (EngineSettings).
    pubsub_itc_fw::ConnectionID outbound_peer_conn_id_;
    pubsub_itc_fw::ConnectionID inbound_peer_conn_id_;

    // Order book keyed by (session identity, cl_ord_id).
    // Built by applying the WAL's commands, whether this instance leads or follows.
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
        void order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                           ReportIsRepeat repeat, int64_t sequence_number, std::string_view orig_cl_ord_id) override;
        void order_accepted() override;

      private:
        MatchingEngineThread& owner_;
    };
    EngineOutputs engine_outputs_{*this};

    // How EngineReportSending sends a report and tells the test.
    class ReportSendingLinks : public EngineReportSendingLinksInterface {
      public:
        explicit ReportSendingLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, const pubsub_itc_fw_app::WalRecord& envelope) override;
        void reports_held(int64_t count) override;
        void reports_released(pubsub_itc_fw_app::ReleaseReason reason, int64_t count) override;
        void reports_discarded(int64_t count) override;

      private:
        MatchingEngineThread& owner_;
    };
    ReportSendingLinks report_sending_links_{*this};
    // Sends each report to both sequencers, numbered, or holds it (section 6.2 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md). Each report goes to both: the leading sequencer routes it
    // to the member's gateway, and the other keeps it in case it takes the lead.
    EngineReportSending report_sending_{config_.ha_enabled, report_sending_links_, get_logger()};

    // How EngineReportPacing pauses and resumes the commands from the sequencers, and asks to be told what is written.
    class ReportPacingLinks : public EngineReportPacingLinksInterface {
      public:
        explicit ReportPacingLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void pause_reading_commands() override;
        void resume_reading_commands() override;
        void request_written_notification(const pubsub_itc_fw::ConnectionID& connection) override;

      private:
        MatchingEngineThread& owner_;
    };
    ReportPacingLinks report_pacing_links_{*this};
    // Keeps the reports sent ahead of what the leading sequencer's connection has written to a fixed number, by pausing
    // the commands arriving (docs/availability/pacing_the_engines_reports.md, BUG-0234).
    EngineReportPacing report_pacing_{report_pacing_links_, get_logger()};
    // What each sequencer last announced on this engine's connection for reports to it: its role and the epoch it holds
    // it under. The connection to the one that says it leads, under the higher epoch if both say so, is paced.
    struct AnnouncedSequencerRole {
        pubsub_itc_fw_app::Role role{pubsub_itc_fw_app::Role::unknown};
        int32_t epoch{0};
    };
    AnnouncedSequencerRole primary_sequencer_role_{};
    AnnouncedSequencerRole secondary_sequencer_role_{};
    // Paces on the connection for reports to the sequencer that leads now, if one is known to.
    void pace_on_leading_sequencer();
    // A sequencer's RoleAnnouncement, sent on this engine's connection for reports to it.
    void handle_sequencer_role_announcement(const pubsub_itc_fw::EventMessage& message);

    // How FollowerReportComparison acts on what it finds.
    class ReportComparisonLinks : public FollowerReportComparisonLinksInterface {
      public:
        explicit ReportComparisonLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void stop_leading_for_good(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason) override;
        void difference_found() override;
        void kept_reports_sent(int64_t sent, int64_t reported_through) override;

      private:
        MatchingEngineThread& owner_;
    };
    ReportComparisonLinks report_comparison_links_{*this};
    // The reports of the commands this instance applies while following the WAL, kept and compared with the leader's, and
    // whether this instance has refused to lead (section 6.3 of docs/venue/dividing_the_sequencer_and_matching_engine.md).
    FollowerReportComparison report_comparison_{report_comparison_links_, get_logger()};
    // How many reports this instance produced while following that differ from the leader's of the same command.
    pubsub_itc_fw::CounterHandle follower_report_differences_counter_;

    // What makes the OrderID and ExecID values, from the secret in identifiers.secret_file. Read when the thread is
    // constructed, so that an engine whose secret cannot be read does not start.
    ReportIdentifiers report_identifiers_{ReportIdentifiers::read_secret_file(config_.identifier_secret_file)};

    // The handling of commands: what each new order and cancel does to the book, and the reports it produces. Declared
    // after the book, the maker of identifiers and the outputs it is given, so that they exist when it is constructed.
    MatchingEngineStateMachine state_machine_{order_book_, report_identifiers_, engine_outputs_, get_logger()};

    // What this instance is doing: starting, catching up, leading, or following, and why (section 6.1 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    EngineRole role_{config_.ha_enabled};

    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already spent.
    int32_t epoch_{0};

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    // The epoch on disk, each record of it timed (BUG-0228).
    pubsub_itc_fw::EpochRecording epoch_recording_;
    // Writes the epoch this instance asks to lead in before the request is sent, through epoch_recording_.
    pubsub_itc_fw::EpochRecordedBeforeAsking epoch_recorded_before_asking_;

    // Where this instance's promises in deciding which instance leads outlive the process, until
    // the machine reboots. It lets a process restarted by its supervisor keep the lead it held.
    pubsub_itc_fw::LeasePromiseStore lease_promise_store_;
    // Writes promise records on a thread of its own, so that refreshing one does not stop this
    // instance answering lease requests while the disk is written (BUG-0107). It is created before
    // the Reactor pins this thread, so its writer inherits the background-core mask.
    std::optional<pubsub_itc_fw::BackgroundPromiseRecorder> background_promise_recorder_;

    // Drives the lease rules that decide whether this instance leads. Recurring.
    pubsub_itc_fw::TimerID lease_tick_timer_id_{};

    // How the lease rules reach the other two voters: the peer instance over whichever connection
    // to it is up, and the arbiter pool on both arbiter connections.
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

    // Why the region holding the open orders could not be used at startup, or empty if it could; and why trading is
    // halted, once it is. Kept for the test, which may connect after either happened (report_present_state).
    std::string region_unusable_reason_;
    std::optional<pubsub_itc_fw_app::HaltReason> halt_reason_;

    // While this instance waits for the TechnicalEvent halting trading that it asked for (EngineCatchUp::halt_awaited),
    // the timer that asks again.
    pubsub_itc_fw::TimerID halt_request_timer_id_{};
    static constexpr std::chrono::seconds halt_request_interval{1};

    void report_order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                              ReportIsRepeat repeat, int64_t sequence_number, std::string_view orig_cl_ord_id = {});

    // Armed while this instance is reconciling, and cancelled by the ack: its ticks go to EngineCatchUp::on_retry_tick.
    pubsub_itc_fw::TimerID reconciliation_timer_id_{};

    // Whether reports are held until this instance knows whether it serves: it is catching up, with high availability
    // on, for a purpose other than leading. An instance that is about to learn its peer leads would otherwise report
    // every recovered order to the member that placed it and then serve nobody. Reporting is an act, and BUG-0064's
    // fifth trap says being current and being entitled to act are separate things.
    [[nodiscard]] bool holding_reports_until_entitled() const;

    // Whether the command being applied came to this instance as a follower of the WAL, so that its reports are not sent.
    bool current_command_follows_{false};
    // How many reports were not sent because their commands were applied while following, for the Quill log.
    int64_t followed_reports_withheld_{0};

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
    // Order connections from the sequencers that are held, unread and not yet treated as made, until this instance knows
    // whether it leads or follows. Empty unless a test sets hold_sequencer_connections_until_role_decided.
    std::vector<pubsub_itc_fw::ConnectionID> held_sequencer_connections_;
    // An order connection from a sequencer is made: this instance records it, and asks to be caught up if it needs to.
    void sequencer_order_connection_established(const pubsub_itc_fw::ConnectionID& id);
    // Whether the order connection @p id is held, for a test that sets hold_sequencer_connections_until_role_decided. If
    // it is, reading from it is paused and it is not treated as made.
    bool holds_sequencer_connection(const pubsub_itc_fw::ConnectionID& id);
    // Whether this instance knows whether it leads or follows: it leads, follows, or is catching up to do either.
    [[nodiscard]] bool role_decided() const;
    // Once this instance knows whether it leads or follows, each held order connection is read again and treated as made.
    void release_held_sequencer_connections();

    // How EngineCatchUp sends its requests and starts its timers.
    class CatchUpLinks : public EngineCatchUpLinksInterface {
      public:
        explicit CatchUpLinks(MatchingEngineThread& owner) : owner_(owner) {}
        void send_follow_request(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequest& request) override;
        void start_retry_timer() override;
        void stop_retry_timer() override;
        void request_halt() override;
        void start_halt_timer() override;
        void stop_halt_timer() override;
        void region_rebuilt(int64_t through) override;

      private:
        MatchingEngineThread& owner_;
    };
    CatchUpLinks catch_up_links_{*this};
    // Asking to be caught up and checking the answer, the engine's position, and an unusable region (section 6.4 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md). Declared after the connections it sends on.
    EngineCatchUp catch_up_{static_cast<int64_t>(config_.instance_id), config_.ha_enabled, sequencer_order_conn_ids_, catch_up_links_, get_logger()};
    // What a catch-up request says that EngineCatchUp does not know: whether one may be asked for, whether to lead, and
    // the last command applied.
    [[nodiscard]] EngineCatchUp::RequestContext catch_up_context() const;

    // Arbiter-mediated promotion helpers.
    void enter_follower_state();
    void adopt_leader_role();
    /// Changes what this instance does to follow what the lease rules have just decided.
    void act_on(pubsub_itc_fw::PairLeaseAgent::Change change);
    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_grant(const pubsub_itc_fw::EventMessage& message);
    void handle_lease_refusal(const pubsub_itc_fw::EventMessage& message);
    /// Either arbiter says it holds this instance's statement about its peer.
    void handle_statement_held(const pubsub_itc_fw::EventMessage& message);

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

    /// Asks to be caught up again and to go on following the WAL, keeping the role this instance has once the catch-up
    /// ends; @p why says for the Quill log what made it ask.
    void follow_the_wal_again(const char* why);

    /// Whether this instance follows the WAL now: high availability is on, it asked to follow, it does not lead, and it is
    /// not catching up.
    [[nodiscard]] bool following_the_wal() const;

    /// Whether this instance keeps the reports of the commands it applies rather than sending them: it follows the WAL,
    /// or is catching up in order to follow it.
    [[nodiscard]] bool keeps_its_reports() const;

    /// Keeps @p report, of command @p seq_no for @p session, until the WAL holds the leader's reports of that command.
    void keep_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t seq_no, const fix_common::SessionIdentity& session);

    /// Taking the lead: sends the kept reports of every command after @p reported_through, the last command the WAL holds
    /// every report of, and drops the rest.
    void send_kept_reports_after(int64_t reported_through);

    /// While following, checks that the record numbered @p seq_no is the next one; if it is not, says so and asks to be
    /// caught up again. Returns whether the record is to be acted on.
    [[nodiscard]] bool next_followed_record(int64_t seq_no);

    /// Take the leader role, catching up from the WAL first unless this instance already leads.
    /// A follower may lack the last few records still on their way to it, and its catch-up to lead
    /// is also when it learns which of its kept reports the WAL lacks. An
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
    // The leading sequencer has started to answer a catch-up request (WalFollowStarted).
    void handle_follow_started(const pubsub_itc_fw::EventMessage& message);
    // The leading sequencer has sent every order after this instance's position (WalFollowCaughtUp).
    void handle_caught_up(const pubsub_itc_fw::EventMessage& message);
    // Whether a record arriving on @p connection is from an earlier answer still on its way, while catching up.
    [[nodiscard]] bool from_an_earlier_answer(const pubsub_itc_fw::ConnectionID& connection) const;
    // A sequencer that does not lead has declined the request (WalFollowDeclined); it says when it leads.
    void handle_follow_declined(const pubsub_itc_fw::EventMessage& message);
    // A sequencer has taken the lead (WalLeaderChanged): a request still unanswered is made again at once.
    void handle_leader_changed(const pubsub_itc_fw::EventMessage& message);
    // A sequencer that has just taken the lead asks for the highest order or cancel this engine has
    // acted on, so that it can send any its log holds above that (EnginePositionQuery, 118).
    void handle_engine_position_query(const pubsub_itc_fw::EventMessage& message);

    // The settings that change what the engine does with a command, which the two engines of a pair must share
    // (docs/availability/deterministic_matching_engine.md, section 5). Sent to the peer on @p to_peer whenever a
    // connection to it is established, in either direction, since lease messages use whichever is up (BUG-0231).
    void send_engine_settings(const pubsub_itc_fw::ConnectionID& to_peer);
    // The peer's settings arrived: given to PeerSettingsCheck.
    void handle_engine_settings(const pubsub_itc_fw::EventMessage& message);

    // How PeerSettingsCheck acts through the lease agent and tells the test.
    class SettingsLinks : public PeerSettingsCheckLinksInterface {
      public:
        explicit SettingsLinks(MatchingEngineThread& owner) : owner_(owner) {}
        [[nodiscard]] bool leading() const override;
        [[nodiscard]] bool says_peer_may_lead() const override;
        void say_peer_may_not_lead() override;
        void say_peer_may_lead() override;
        void settings_differ(int64_t own_capacity, int64_t peer_capacity, bool barred_peer, bool secrets_differ) override;

      private:
        MatchingEngineThread& owner_;
    };
    SettingsLinks settings_links_{*this};
    // The settings the two engines of a pair must share, compared (section 6.6 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md). Made at the end of the constructor, once the open-order
    // region is open and its capacity known.
    std::optional<PeerSettingsCheck> peer_settings_check_;

    // Applies a CancelBook command, and tells the test what it cancelled.
    void apply_cancel_book_command(const pubsub_itc_fw_app::CancelBookView& command, int64_t sequence_number, int64_t transact_time, int32_t trading_date,
                                   CommandSource source);
    // Applies a TechnicalEvent command, and tells the test of a halt it set.
    void apply_technical_event_command(const pubsub_itc_fw_app::TechnicalEventView& event, int64_t sequence_number, int32_t trading_date, CommandSource source);
    // The leading sequencer sent the voids in its WAL (VoidedCommands): each is added to those the state machine knows
    // of, and one learned of after applying a command it names stops this instance leading
    // (docs/availability/voided_commands.md, section 3.4).
    void handle_voided_commands(const pubsub_itc_fw::EventMessage& message);
    // Applies a VoidCommands command, and stops this instance leading if it was not told of the void in time.
    void apply_void_commands_command(const pubsub_itc_fw_app::VoidCommandsView& command, int64_t sequence_number, CommandSource source);
    // Tells the test that this instance resumed as leader, and with how many orders.
    void report_resumed_as_leader();

    // The position the last catch-up before leading reached, which the test is told when this instance resumes.
    int64_t resumed_through_{0};

    /**
     * @brief Whether the engine can say what it is holding.
     *
     * The region vouches for the book. Where the one this process inherited could not be used, the
     * engine asks for every record from the first, and holds what that replay rebuilt: the venue's
     * book, if the WAL reached the first command of the trading day, and otherwise an incomplete one
     * (R-0102, R-0123). See account_for_unusable_region.
     */
    [[nodiscard]] bool book_can_be_vouched_for() const;

    // Asks every sequencer for a TechnicalEvent halting trading because this instance cannot account for the book; the
    // leading one writes it. Asked again every halt_request_interval until trading is halted.
    void request_halt();

    // Applies one command from the WAL: a new order, a cancel, a CancelBook, a TechnicalEvent or a VoidCommands. Records in
    // the region that the command is being applied before it applies it, and that none is once it has applied it and
    // sent its reports, so that a process that dies applying it is known to have done so (DiedApplyingCheck).
    void apply_command(const pubsub_itc_fw::EventMessage& message, const pubsub_itc_fw_app::WalRecordView& envelope, pubsub_itc_fw::BumpAllocator& arena);

    // Aborts the process if a test named this order's ClOrdID in [test_events] die_applying_cl_ord_id, and the order is
    // about to be applied rather than voided or passed over (docs/availability/voided_commands.md, section 8, step 2).
    void die_if_named(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number);

    // If a test named this order's ClOrdID in [test_events] lose_and_regain_lease_after_cl_ord_id, and this instance
    // leads, starts lose_and_regain_lease_timer_id_, once in the life of the process, so that just after the order is
    // applied the instance acts as if its lease ran out and was at once granted again (BUG-0218).
    void lose_and_regain_lease_if_named(const pubsub_itc_fw_app::NewOrderSingleView& view);
    bool lost_and_regained_lease_{false};
    pubsub_itc_fw::TimerID lose_and_regain_lease_timer_id_{};

    // ---- A command the process before this one died applying (docs/availability/voided_commands.md, section 4) ----

    // How DiedApplyingCheck asks its questions.
    class DiedApplyingLinks : public DiedApplyingCheckLinksInterface {
      public:
        explicit DiedApplyingLinks(MatchingEngineThread& owner) : owner_(owner) {}
        pubsub_itc_fw::ConnectionID ask_peer_which_commands_it_applied() override;
        void start_peer_answer_timer() override;
        void stop_peer_answer_timer() override;
        void send_void_request(int64_t command) override;
        void start_void_request_timer() override;
        void stop_void_request_timer() override;
        void settled(int64_t command, pubsub_itc_fw_app::DiedApplyingOutcome outcome) override;

      private:
        MatchingEngineThread& owner_;
    };
    DiedApplyingLinks died_applying_links_{*this};
    // Whether to ask for the command the previous process died applying to be voided (section 6.5 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md). Until the question is settled, no catch-up is asked for.
    DiedApplyingCheck died_applying_check_{died_applying_links_, get_logger()};
    pubsub_itc_fw::TimerID peer_answer_timer_id_{};
    pubsub_itc_fw::TimerID void_request_timer_id_{};
    static constexpr std::chrono::seconds void_request_interval{1};

    // The peer asks for the highest command this instance has applied (AppliedThroughQuery): it is told.
    void handle_applied_through_query(const pubsub_itc_fw::EventMessage& message);
    // The peer's answer (AppliedThrough): given to DiedApplyingCheck.
    void handle_applied_through(const pubsub_itc_fw::EventMessage& message);
    // The leading sequencer will not void the command (VoidDeclined): given to DiedApplyingCheck.
    void handle_void_declined(const pubsub_itc_fw::EventMessage& message);
};

} // namespaces
