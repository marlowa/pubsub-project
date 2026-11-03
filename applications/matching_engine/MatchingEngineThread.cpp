// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include <LeasePairNumbers.hpp>
#include <OrderPathMetrics.hpp>
#include <lease_messages.hpp>
#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LeaderEpoch.hpp>
#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace matching_engine {

namespace {

// The arbiter pool's identity as a voter in deciding which matching engine leads. The engines are 1 and 2.
constexpr int64_t arbiter_pool_voter_id = 3;

// Which session an inbound envelope belongs to.
//
// The comp id is the identity; the connection id that also rides on the envelope is only
// where that session happened to be when it sent this, and is deliberately not consulted
// here. An envelope with no comp id yields an empty identity: that is a record with no
// originating client session at all, and it keys nothing.
template <typename EnvelopeT> fix_common::SessionIdentity session_identity_from(const EnvelopeT& envelope) {
    if (!envelope.has_sender_comp_id || envelope.sender_comp_id.empty()) {
        return fix_common::SessionIdentity{};
    }
    return fix_common::SessionIdentity::make(envelope.sender_comp_id,
                                             envelope.has_origin_gateway_id ? envelope.origin_gateway_id : gateway_ids::default_when_absent);
}

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

// A named helper rather than a designated initialiser at the call site: this project builds
// as C++17, where designated initialisers are a C++20 feature and -Werror rejects them.
pubsub_itc_fw::ApplicationThreadConfiguration make_thread_config() {
    pubsub_itc_fw::ApplicationThreadConfiguration configuration;
    configuration.metrics_scope = "matching_engine_thread";
    return configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const MatchingEngineConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "MatchingEnginePool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "MatchingEnginePool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // un-named namespace

MatchingEngineThread::MatchingEngineThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                           pubsub_itc_fw::Reactor& reactor, const MatchingEngineConfiguration& config)
    : ApplicationThread(token, logger, reactor, "MatchingEngineThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        make_thread_config())
    , config_(config)
    , ha_enabled_(config.ha_enabled)
    , is_primary_(!config.ha_enabled || config.ha_role == "primary")
    , order_book_(&book_growth_reporter_)
    , epoch_store_(config.epoch_state_file)
    , lease_promise_store_(config.epoch_state_file + ".lease_promise", pubsub_itc_fw::LeasePromiseStore::current_boot_id()) {
    // Wired before the reserve below, so even the initial capacity is reported if it is
    // large. The book is the venue's biggest consumer of memory and was, until this,
    // completely uninstrumented: the pool and slab allocators cover objects with a message
    // lifecycle, and the book is neither, so it grew to 9.9 GB on the OS heap and the
    // process was OOM-killed having logged no memory warning at all.
    //
    // IncrementalRehashMap allocates a whole table at a time, so this fires roughly two dozen
    // times over the life of a process, never per order. Note that during a migration the map
    // holds two tables at once, so peak resident size is larger than the reported figure for
    // the new table alone -- the reporter names what was just allocated, not the total.
    book_growth_reporter_.report_threshold_bytes = static_cast<size_t>(config_.order_book_growth_report_threshold_bytes);
    book_growth_reporter_.on_large_allocation = [&logger](size_t bytes, size_t largest) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: order book storage growing -- allocating {} MB (largest so far {} MB). "
                   "The book is not bounded; sustained growth ends in the OOM killer.",
                   bytes / (1024UL * 1024UL), largest / (1024UL * 1024UL));
    };

    // The region holds the orders themselves and is sized to the most that may be open at
    // once; the map beside it is sized to the same expectation but may grow past it.
    bool region_existed = false;
    try {
        region_existed = order_book_.open(config_.order_book_region_path, static_cast<OrderBook::SlotIndex>(config_.order_book_region_capacity),
                                          static_cast<size_t>(config_.order_book_initial_capacity));
    } catch (const pubsub_itc_fw::PubSubItcException& error) {
        // Damaged, or written by a different build. R-0102 says such a record is not to be
        // used, and it is not: it is moved aside intact so that somebody can look at it, and a
        // new one is started. What was in it is now unknown to this process, so the book cannot
        // be vouched for and trading must not simply resume -- R-0123 decides what happens
        // instead, once the sequencer's record has been asked.
        catch_up_.region_found_unusable();
        region_unusable_reason_ = error.what();
        const std::string kept = config_.order_book_region_path + ".unusable";
        const bool moved = std::rename(config_.order_book_region_path.c_str(), kept.c_str()) == 0;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the order book region at {} cannot be used -- {}. It is {} and a new one started; what it held will be established "
                   "from the sequencer's record instead (R-0102, R-0123)",
                   config_.order_book_region_path, error.what(), moved ? "kept at " + kept : "left where it is");
        region_existed = order_book_.open(config_.order_book_region_path, static_cast<OrderBook::SlotIndex>(config_.order_book_region_capacity),
                                          static_cast<size_t>(config_.order_book_initial_capacity));
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: order book region {} at {} holding up to {} order(s)",
               region_existed ? "opened" : "created", config_.order_book_region_path, order_book_.region_capacity());

    recover_open_orders(region_existed);

    // Registered unconditionally: this component has exactly one application thread and
    // names its scope above, so there is no second thread to collide with. The handle is a
    // no-op when metrics are disabled, and the application and component tokens come from
    // configuration rather than from here. See docs/operations/metrics.md.
    orders_processed_counter_ = get_reactor().metrics().register_counter("matching_engine_thread", "orders_processed_total",
                                                                         "New orders accepted onto the book by the matching engine");
    follower_report_differences_counter_ =
        get_reactor().metrics().register_counter("matching_engine_thread", "follower_report_differences_total",
                                                 "Reports this engine produced while following that differ from the leader's of the same command");

    // HA classification. A non-HA ME is a plain single instance (Unknown state,
    // full processing). With HA on, both instances begin Unknown, whichever is configured as
    // primary: each catches up from the WAL when the first sequencer connects, and leads or
    // follows only once a majority has settled which (BUG-0195). The state is logged so that
    // someone reading a log from a machine that has just come up can see what it thinks it is
    // before anything else happens.
    if (ha_enabled_) {
        // Recover the leadership generation before anything can elect. Without this
        // the node starts at zero, and a pair restarted together would agree on a
        // generation the venue has already used -- see pubsub_itc_fw::EpochStore.
        epoch_ = epoch_store_.load();

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: HA enabled, starting as {} (instance_id={}, configured {}, epoch={})",
                   me_role_name(role_.role()), config_.instance_id, is_primary_ ? "primary" : "secondary", epoch_);
        if (epoch_ > 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: recovered epoch={} from {}", epoch_, epoch_store_.path());
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: no stored epoch at {} -- starting from epoch 0",
                       epoch_store_.path());
        }
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: HA disabled -- single instance, no arbitration and no peer");
    }
}

void MatchingEngineThread::on_initial_event() {
    // The constructor opened and recovered the book on the thread that built this engine. The
    // first event delivered on this thread is this one, so the book is handed to it here, before
    // anything on this thread uses it.
    order_book_.hand_over_ownership();
}

void MatchingEngineThread::on_app_ready_event() {
    // Registered here rather than in the constructor because the reactor's metrics
    // endpoint is what gauges bind to, and it is ready by this point.
    book_metrics_.register_metrics(get_reactor().metrics(), book_metrics_scope);

    if (!config_.order_path_elapsed_buckets.empty()) {
        order_in_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::order_in_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
        er_out_elapsed_histogram_ =
            get_reactor().metrics().register_histogram(order_path_metrics::er_out_scope, order_path_metrics::order_path_elapsed_metric_name,
                                                       order_path_metrics::order_path_elapsed_help, config_.order_path_elapsed_buckets);
    }
    book_metrics_timer_id_ = start_recurring_timer(book_metrics_sample_interval);
    publish_book_metrics();

    // Only when a test started this instance (docs/availability/test_events.md).
    if (config_.test_events.enabled) {
        test_events_.emplace(test_event_links_, config_.test_events.component, static_cast<int64_t>(config_.instance_id), "MatchingEngineThread");
        connect_to_service(std::string(pubsub_itc_fw::TestEventChannel::service_name));
    }

    // Both roles connect outbound to the sequencer ER listeners (pre-warmed so
    // ERs, including the cancel-on-failover burst, flow immediately on promotion).
    connect_to_service("sequencer_er");
    connect_to_service("sequencer_er_secondary");

    if (ha_enabled_) {
        // Both roles dial, and both listen: holding both connections to the peer permanently means a role change costs
        // no connection work at the moment the venue can least afford it.
        connect_to_service("me_peer");
    }

    if (ha_enabled_) {
        const int64_t self_id = static_cast<int64_t>(config_.instance_id);
        lease_agent_.emplace("MatchingEngineThread", get_logger(), lease_links_, fix_common::pair_id_of(pubsub_itc_fw_app::ComponentGroup::matching_engine),
                             self_id, peer_instance_id(), arbiter_pool_voter_id, "the arbiter", config_.lease, std::chrono::steady_clock::now(), epoch_,
                             pubsub_itc_fw::PeerStatementsFlag{pubsub_itc_fw::PeerStatementsFlag::SayWhenPeerMayNotLead});
        if (test_events_.has_value()) {
            lease_agent_->report_test_events_to(*test_events_);
        }
        background_promise_recorder_.emplace(lease_promise_store_);
        lease_agent_->keep_promises_in(*background_promise_recorder_, lease_promise_store_.load(), lease_promise_store_.load_statement(),
                                       std::chrono::steady_clock::now());
        lease_tick_timer_id_ = start_recurring_timer(pubsub_itc_fw::LeaseTiming::tick_interval);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: leading only while the peer or the arbiter grants a lease (period={} ms); nothing is asked for during the first "
                   "period",
                   config_.lease.period.count());
    }

    if (ha_enabled_) {
        // Both roles connect to the arbiter pool, which is one of the three voters.
        connect_to_service("arbiter_primary");
        connect_to_service("arbiter_secondary");
    }

    begin_died_applying_question();
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (test_events_.has_value() && test_events_->on_connection_established(id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: connection {} to the test's event listener established",
                   id.get_value());
        // A role taken, or a catch-up begun or finished, before the connection was made is reported now, so that the test
        // learns it whenever it connects.
        report_present_state();
        if (role_.role() == MeRole::Leader || role_.role() == MeRole::Follower) {
            report_role_change(MeRole::Unknown, role_.role());
        }
        return;
    }
    const std::string& svc = id.service_name();
    const std::string order_inbound_svc = "inbound:" + std::to_string(config_.listen_port);
    const std::string peer_inbound_svc = "inbound:" + std::to_string(config_.peer_listen_port);

    if (svc == "sequencer_er") {
        report_sending_.connection_established(EngineReportSending::Sequencer::primary, id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: primary sequencer ER connection {} established", id.get_value());
        announce_role();
        report_sending_.release_for_connection(epoch_, holding_reports_until_entitled());
    } else if (svc == "sequencer_er_secondary") {
        report_sending_.connection_established(EngineReportSending::Sequencer::secondary, id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
        announce_role();
        report_sending_.release_for_connection(epoch_, holding_reports_until_entitled());
    } else if (svc == "me_peer") {
        // Our dial to the peer's listener, for the lease requests between the pair, the role announcements and the
        // settings check.
        outbound_peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: connection {} to the peer engine established", id.get_value());
        send_engine_settings();
        ask_peer_whether_applied();
        // Tell the peer what this instance is as soon as there is a link to tell it
        // on. An instance that has just restarted is sitting in Unknown waiting for
        // arbitration, and a leader saying so here lets it settle immediately rather
        // than waiting on an arbiter that may decline while it is still learning.
        announce_role();
    } else if (svc == "arbiter_primary") {
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-primary connection {} established", id.get_value());
    } else if (svc == "arbiter_secondary") {
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-secondary connection {} established", id.get_value());
    } else if (ha_enabled_ && svc == peer_inbound_svc) {
        // The peer's dial to us, which carries the same as ours.
        inbound_peer_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: connection {} from the peer engine established", id.get_value());
        ask_peer_whether_applied();
        // Tell the peer what this instance is as soon as there is a link to tell it
        // on. An instance that has just restarted is sitting in Unknown waiting for
        // arbitration, and a leader saying so here lets it settle immediately rather
        // than waiting on an arbiter that may decline while it is still learning.
        announce_role();
    } else {
        // An inbound order connection from a sequencer, alike for either configured role.
        sequencer_order_conn_ids_.insert(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound sequencer order connection {} established", id.get_value());
        if (died_applying_question_ == DiedApplyingQuestion::asking_sequencers) {
            send_void_request();
        }
        if (role_.role() == MeRole::Reconciling) {
            // Either reconciliation was begun before any sequencer had dialled in, so the request
            // could not be sent, and sending it now is what keeps that from stranding the venue
            // with no engine leading -- the outage BUG-0043 recorded; or it was sent on another
            // connection, and only this one is still to be asked.
            ask_on_new_sequencer_connection(id);
        } else if (!role_.catch_up_completed() && role_.role() != MeRole::Follower) {
            // The catch-up belongs here, at the moment this instance first has somewhere to ask
            // and before it has served anything, rather than later when a role is settled. An
            // instance that starts and works for fifteen seconds while an arbiter makes up its
            // mind has already applied those orders; it is the orders taken BEFORE it started
            // that it has not seen, and this is the only moment it can ask about them without
            // mistaking live traffic for replay.
            //
            // An instance that already follows is excluded: it follows the WAL, and asks again itself when it needs to.
            //
            // An order in the venue's WAL that no engine has applied, such as one a sequencer
            // wrote and then died before sending on, is in no engine's book, so this ask is the
            // only thing that recovers it. See BUG-0064.
            //
            // Becoming current, not taking the role: who may act is the arbiter's to say, and
            // an instance that promoted itself on the strength of having caught up would be
            // deciding that for itself.
            role_.start_catching_up_to_become_current();
            begin_reconciliation();
        }
    }
}

void MatchingEngineThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (test_events_.has_value() && test_events_->on_connection_lost(id, reason)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: the connection to the test's event listener is lost ({}) -- no more test events are sent", reason);
        return;
    }
    if (id == report_sending_.connection(EngineReportSending::Sequencer::primary)) {
        report_sending_.connection_lost(EngineReportSending::Sequencer::primary);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: primary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == report_sending_.connection(EngineReportSending::Sequencer::secondary)) {
        report_sending_.connection_lost(EngineReportSending::Sequencer::secondary);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: secondary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == outbound_peer_conn_id_) {
        outbound_peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: connection {} to the peer engine lost: {}", id.get_value(), reason);
    } else if (id == inbound_peer_conn_id_) {
        inbound_peer_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: connection {} from the peer engine lost: {}", id.get_value(),
                   reason);
        // Nothing is armed here. Losing the link is not how a follower decides its leader has gone:
        // the lease rules decide, when the promise this instance made to the leader runs out.
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
    } else if (sequencer_order_conn_ids_.count(id) > 0) {
        sequencer_order_conn_ids_.erase(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound sequencer order connection {} lost: {}", id.get_value(),
                   reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEngineThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    if (test_events_.has_value() && test_events_->on_framework_pdu_message(message)) {
        release_pdu_payload(message);
        return;
    }
    const auto pdu_id = message.pdu_id();

    // Forgotten here, at the start of every message, and remembered again further down only
    // where a client order is being handled. Clearing it first is what stops the time
    // belonging to one order being read while a later, unrelated message is being processed.
    has_current_order_ingress_ns_ = false;
    current_order_ingress_ns_ = 0;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: sequenced PDU received on connection {} pdu_id={}",
               message.connection_id().get_value(), pdu_id);

    // ---- HA control PDUs (Slice C+D) --------------------------------------
    if (pdu_id == pubsub_itc_fw::LeaseRequest::message_pdu_id) {
        handle_lease_request(message.connection_id(), message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw::LeaseGrant::message_pdu_id) {
        handle_lease_grant(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw::LeaseRefusal::message_pdu_id) {
        handle_lease_refusal(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::RoleAnnouncement::message_pdu_id) {
        // The peer saying which role it holds. Nothing to act on: which instance leads is decided
        // by leases, and the announcement is for the sequencers, which route orders by it.
        release_pdu_payload(message);
        return;
    }

    if (pdu_id == pubsub_itc_fw::WalFollowStarted::message_pdu_id) {
        handle_follow_started(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw::WalFollowCaughtUp::message_pdu_id) {
        handle_caught_up(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw::WalFollowDeclined::message_pdu_id) {
        handle_follow_declined(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw::WalLeaderChanged::message_pdu_id) {
        handle_leader_changed(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::EnginePositionQuery::message_pdu_id) {
        handle_engine_position_query(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::EngineSettings::message_pdu_id) {
        handle_engine_settings(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::VoidedCommands::message_pdu_id) {
        handle_voided_commands(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::VoidDeclined::message_pdu_id) {
        handle_void_declined(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::AppliedThroughQuery::message_pdu_id) {
        handle_applied_through_query(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::AppliedThrough::message_pdu_id) {
        handle_applied_through(message);
        release_pdu_payload(message);
        return;
    }

    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported sequenced PDU id {} -- dropping", pdu_id);
        release_pdu_payload(message);
        return;
    }
    if (following_the_wal() && !next_followed_record(message.seq_no())) {
        release_pdu_payload(message);
        return;
    }
    if (from_an_earlier_answer(message.connection_id())) {
        catch_up_.ignored_a_record_from_an_earlier_answer();
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: record seq={} on connection {} is from an earlier answer to a catch-up request -- ignored", message.seq_no(),
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    // Orders arrive wrapped in a WalRecord envelope from the sequencer: the routing
    // and sequencing metadata rides on the envelope, the DD-derived FIX PDU is the
    // payload. Unwrap the envelope, then dispatch the inner NOS/OCR using the
    // envelope's wall_time_ns as the sequencing time (transact_time during replay).
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode order envelope -- dropping");
        release_pdu_payload(message);
        return;
    }

    const int16_t inner_pdu_id = envelope.pdu_id;
    // A CancelBook, a TechnicalEvent or a VoidCommands command is gated as an order is: it changes the book, whether
    // trading is halted, or which commands are applied, so it is applied live only by the instance that leads, and by a
    // follower as it follows the WAL.
    const bool is_order_pdu = (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id));

    // ---- Order PDU gating by HA state -------------------------------------
    if (is_order_pdu && role_.role() == MeRole::Follower && !following_the_wal()) {
        // A follower applies only what it is sent as a follower of the WAL. A command reaching it otherwise, from order
        // routing before the sequencer learned it follows, is in the WAL, and reaches it as a follower in its turn.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: FOLLOWER -- discarding order PDU inner_pdu_id={} on connection {}",
                   inner_pdu_id, message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }
    if (is_order_pdu && ha_enabled_ && role_.role() == MeRole::Unknown && !following_the_wal()) {
        // With high availability on, Unknown means this instance holds no lease: it has not yet
        // been granted one, or it has caught up and is waiting to be. It must not match orders,
        // or an instance that no majority chose would be acting (R-0146). Nothing is lost by
        // discarding them: they are in the sequencer's log, and whichever instance leads catches
        // up from that log before it acts. With high availability off, Unknown is the ordinary
        // state of the single instance, and it processes everything.
        //
        // Discarding one means this instance is no longer current, even if it caught up a moment
        // ago. So it is marked as needing a catch-up, and becoming leader then catches up again,
        // including this order, before it acts.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: holding no lease -- discarding order PDU inner_pdu_id={} on connection {}", inner_pdu_id,
                   message.connection_id().get_value());
        role_.command_discarded_without_a_lease();
        release_pdu_payload(message);
        return;
    }

    if (is_order_pdu) {
        apply_command(message, envelope, arena);
    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
        // A report record: the engine produces reports and applies none. One that follows the WAL is sent every record,
        // so during its catch-up a report record is one of the records sent, and is counted as arrived (R-0101).
        if (role_.role() == MeRole::Reconciling) {
            catch_up_.record_offered(message.seq_no());
        }
        if (keeps_its_reports() && envelope.has_answers_seq_no) {
            report_comparison_.leader_report(message.seq_no(), envelope.answers_seq_no, envelope.payload.data, envelope.payload.size,
                                             catch_up_.reports_in_wal_through());
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: ExecutionReport envelope on connection {} -- not applied (the engine produces reports and applies none)",
                   message.connection_id().get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported envelope inner pdu_id {} -- dropping", inner_pdu_id);
    }

    release_pdu_payload(message);
}

void MatchingEngineThread::apply_command(const pubsub_itc_fw::EventMessage& message, const pubsub_itc_fw_app::WalRecordView& envelope,
                                         pubsub_itc_fw::BumpAllocator& arena) {
    const int16_t inner_pdu_id = envelope.pdu_id;
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    // Recorded before anything is applied, and cleared only after the command's reports are sent, so that a process that
    // dies anywhere in between leaves the command in the region for the next one to find (died_applying_).
    order_book_.set_applying(message.seq_no());

    if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        pubsub_itc_fw_app::NewOrderSingleView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode NewOrderSingle -- dropping");
        } else {
            // Before the matching work rather than after it, so the stage this closes is the
            // journey to here and the stage it opens is what this component does. New orders
            // only, which is the same set of orders the round trip is measured over; a cancel
            // arriving here is a different journey and belongs to no round trip.
            order_path_metrics::observe_checkpoint(order_in_elapsed_histogram_, envelope.has_gateway_ingress_ns, envelope.gateway_ingress_ns,
                                                   config_.wall_clock->now_ns());

            // Remembered for as long as this one order is being handled. Any acknowledgement the
            // matching engine produces from here until this message is finished with was caused
            // by this order, which is how send_er_to_sequencer can time the acknowledgement
            // leaving without the state machine having to carry the value.
            has_current_order_ingress_ns_ = envelope.has_gateway_ingress_ns;
            current_order_ingress_ns_ = envelope.gateway_ingress_ns;

            const CommandSource source = command_source_for(message.seq_no());
            die_if_named(view, message.seq_no());
            state_machine_.apply_new_order(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0,
                                           session_identity_from(envelope), source);
            lose_and_regain_lease_if_named(view);
        }

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        pubsub_itc_fw_app::OrderCancelRequestView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode OrderCancelRequest -- dropping");
        } else {
            const CommandSource source = command_source_for(message.seq_no());
            state_machine_.apply_cancel(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0,
                                        session_identity_from(envelope), source);
        }

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)) {
        pubsub_itc_fw_app::CancelBookView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode CancelBook -- dropping");
        } else {
            const CommandSource source = command_source_for(message.seq_no());
            apply_cancel_book_command(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0, source);
        }

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)) {
        pubsub_itc_fw_app::TechnicalEventView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode TechnicalEvent -- dropping");
        } else {
            const CommandSource source = command_source_for(message.seq_no());
            apply_technical_event_command(view, message.seq_no(), envelope.has_trading_date ? envelope.trading_date : 0, source);
        }

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::VoidCommands::message_pdu_id)) {
        pubsub_itc_fw_app::VoidCommandsView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode VoidCommands -- dropping");
        } else {
            const CommandSource source = command_source_for(message.seq_no());
            apply_void_commands_command(view, message.seq_no(), source);
        }
    }

    order_book_.set_applying(0);
}

void MatchingEngineThread::die_if_named(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number) {
    const std::string& named = config_.test_events.die_applying_cl_ord_id;
    if (named.empty() || view.cl_ord_id != named) {
        return;
    }
    // A command applied already is passed over, and a voided one is rejected, so neither would reach what kills it.
    if (sequence_number <= state_machine_.highest_applied() || state_machine_.voids().voiding(sequence_number, fix_common::VoidedKind::new_order) != nullptr) {
        return;
    }
    if (config_.test_events.die_applying_once && died_applying_ == sequence_number) {
        return;
    }
    // Written straight to the standard error: the Quill log is written by a thread of its own, which the abort stops.
    fmt::print(stderr, "MatchingEngineThread: dying applying the new order {} at seq={}, as [test_events] die_applying_cl_ord_id says\n", named,
               sequence_number);
    std::fflush(stderr);
    std::abort();
}

void MatchingEngineThread::lose_and_regain_lease_if_named(const pubsub_itc_fw_app::NewOrderSingleView& view) {
    const std::string& named = config_.test_events.lose_and_regain_lease_after_cl_ord_id;
    if (named.empty() || view.cl_ord_id != named || lost_and_regained_lease_ || !ha_enabled_ || role_.role() != MeRole::Leader) {
        return;
    }
    lost_and_regained_lease_ = true;
    // From a timer rather than here, so that the order is finished with first, as it is when a lease runs out between
    // two orders.
    lose_and_regain_lease_timer_id_ = start_one_off_timer(std::chrono::milliseconds{1});
}

CommandSource MatchingEngineThread::command_source_for(int64_t sequence_number) {
    current_command_follows_ = keeps_its_reports();
    if (following_the_wal()) {
        // Applied as a catch-up applies a command: one applied already is passed over, and the book is published after
        // every command. None of its reports is sent: the leader's are the ones the members are sent.
        return CommandSource::catch_up;
    }
    if (role_.role() != MeRole::Reconciling) {
        return CommandSource::live;
    }
    // Counted before the state machine decides anything, a refusal included: the catch-up is complete
    // when every record has been offered, and a record refused before it was counted would leave it
    // looking incomplete for ever (R-0101).
    catch_up_.record_offered(sequence_number);
    return CommandSource::catch_up;
}

void MatchingEngineThread::EngineOutputs::send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number,
                                                      const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    if (owner_.current_command_follows_) {
        ++owner_.followed_reports_withheld_;
        if (sequence_number > 0 && sequence_number <= owner_.catch_up_.reports_in_wal_through()) {
            owner_.catch_up_.report_left_out(); // the WAL holds every report of this command already
            return;
        }
        owner_.keep_report(report, sequence_number, session);
        return;
    }
    // Every report a catch-up produces is marked as a possible repeat, and no other report is. One for a command the WAL
    // already holds every report of is not sent: the member has it, or is sent it, by the sequencers.
    if (repeat == ReportIsRepeat::yes) {
        if (sequence_number > 0 && sequence_number <= owner_.catch_up_.reports_in_wal_through()) {
            owner_.catch_up_.report_left_out();
            return;
        }
        owner_.catch_up_.report_sent();
    }
    owner_.send_er_to_sequencer(report, sequence_number, session, repeat);
}

void MatchingEngineThread::EngineOutputs::order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                                                        pubsub_itc_fw_app::OrderOutcomeKind outcome, ReportIsRepeat repeat, int64_t sequence_number,
                                                        std::string_view orig_cl_ord_id) {
    owner_.report_order_outcome(session, cl_ord_id, outcome, repeat, sequence_number, orig_cl_ord_id);
}

void MatchingEngineThread::EngineOutputs::order_accepted() {
    // Counts orders accepted onto the book, not order PDUs arriving: a catch-up applies orders to the
    // book without this, and a follower discards them, so counting arrivals would make the series jump
    // by the whole replayed backlog at each change of leader and stop meaning orders the venue processed.
    owner_.orders_processed_counter_.increment();
}

bool MatchingEngineThread::holding_reports_until_entitled() const {
    // Only a catch-up done at startup. A promotion's catch-up is a different case: that instance
    // has been told it will lead, and its reports go out as they are produced. With high
    // availability off there is no peer to defer to and nothing to wait for.
    return role_.holding_reports_until_entitled();
}

void MatchingEngineThread::send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no, const fix_common::SessionIdentity& session,
                                                ReportIsRepeat repeat) {
    // Encode the ER, then wrap it in a WalRecord envelope. The echoed seq_no travels in
    // the transport header, which is how the leading sequencer usually routes an ER: it looks
    // the order's sequence up and finds the session that placed it. The session identity on
    // the envelope says whose order the report is about without saying where that member is,
    // which the ME cannot know and which may have changed. It is what routes a report with
    // no originating sequence, such as the seq_no==0 cancel-on-failover reports, and a report
    // a sequencer kept while it was not leading, which has no record of who sent which order.
    // Measure then fit: a zero-size out buffer makes encode report bytes_needed, then
    // the reusable buffer is grown to hold it -- no fixed cap that could silently drop
    // an over-large ER, and no per-ER allocation once the buffer reaches its high-water
    // mark.
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    [[maybe_unused]] const bool measured = pubsub_itc_fw_app::encode(er, nullptr, 0, bytes_written, bytes_needed);
    if (er_encode_buffer_.size() < bytes_needed) {
        er_encode_buffer_.resize(bytes_needed);
    }
    if (!pubsub_itc_fw_app::encode(er, er_encode_buffer_.data(), er_encode_buffer_.size(), bytes_written, bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "MatchingEngineThread: failed to encode ExecutionReport ({} bytes needed) -- not sent",
                   bytes_needed);
        return;
    }

    // Sent to both sequencers, or held: while this instance is becoming current without having been told it may serve,
    // or while there is no connection for reports. Only a report sent now is timed.
    const EngineReportSending::Report report{
        er_encode_buffer_.data(), bytes_written, seq_no, &session, repeat == ReportIsRepeat::yes, has_current_order_ingress_ns_, current_order_ingress_ns_};
    if (!report_sending_.send(report, epoch_, holding_reports_until_entitled())) {
        return;
    }
    if (er.ord_status == pubsub_itc_fw_app::OrdStatus::New) {
        order_path_metrics::observe_checkpoint(er_out_elapsed_histogram_, has_current_order_ingress_ns_, current_order_ingress_ns_,
                                               config_.wall_clock->now_ns());
    }
}

void MatchingEngineThread::ReportSendingLinks::send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no,
                                                           const pubsub_itc_fw_app::WalRecord& envelope) {
    owner_.send_pdu(connection, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq_no, envelope,
                    pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
}

void MatchingEngineThread::ReportSendingLinks::reports_held(int64_t count) {
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::ReportsHeld::message_pdu_id, pubsub_itc_fw_app::ReportsHeld{count});
    }
}

void MatchingEngineThread::ReportSendingLinks::reports_released(pubsub_itc_fw_app::ReleaseReason reason, int64_t count) {
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::ReportsReleased::message_pdu_id, pubsub_itc_fw_app::ReportsReleased{reason, count});
    }
}

void MatchingEngineThread::ReportSendingLinks::reports_discarded(int64_t count) {
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::ReportsDiscarded::message_pdu_id, pubsub_itc_fw_app::ReportsDiscarded{count});
    }
}

void MatchingEngineThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == book_metrics_timer_id_) {
        publish_book_metrics();
        return;
    }

    if (id == lose_and_regain_lease_timer_id_) {
        // What the lease agent reports when the lease runs out and the peer grants another before the request to follow
        // is answered, one after the other, as on the death of both arbiters (BUG-0218).
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: acting as if the lease ran out and was at once granted again, as [test_events] "
                       "lose_and_regain_lease_after_cl_ord_id says");
        act_on(pubsub_itc_fw::PairLeaseAgent::Change::StoppedLeading);
        act_on(pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader);
        return;
    }

    if (id == reconciliation_timer_id_) {
        catch_up_.on_retry_tick(role_.role() == MeRole::Reconciling, catch_up_context());
        return;
    }

    if (id == peer_answer_timer_id_) {
        peer_answer_timer_id_ = pubsub_itc_fw::TimerID{};
        if (died_applying_question_ == DiedApplyingQuestion::asking_peer) {
            ask_for_void("the peer engine did not say within a lease period which commands it has applied");
        }
        return;
    }

    if (id == void_request_timer_id_) {
        // A request lost to a change of sequencer leader, or made before any sequencer led, is asked for again.
        if (died_applying_question_ == DiedApplyingQuestion::asking_sequencers) {
            send_void_request();
        }
        return;
    }

    if (id == halt_request_timer_id_) {
        catch_up_.on_halt_tick(state_machine_.halted());
        return;
    }

    if (id == lease_tick_timer_id_) {
        act_on(lease_agent_->on_tick(std::chrono::steady_clock::now()));
        return;
    }
}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

// HA state machine (Slice C+D)

void MatchingEngineThread::publish_book_metrics() {
    // capacity() spans both tables while the book is being moved into a larger one,
    // so it steps up during a migration and back down when the old table goes. That
    // is the memory genuinely held at that moment, and migrating_ is published
    // alongside it so a step in slots is not mistaken for a step in the book.
    book_metrics_.update(order_book_.size(), order_book_.capacity(), order_book_.is_migrating(),
                         book_growth_reporter_.largest_allocation_bytes.load(std::memory_order_relaxed));
}

void MatchingEngineThread::set_epoch(int32_t new_epoch) {
    if (new_epoch < epoch_) {
        // Nothing should ask for this. Refusing keeps the counter monotonic even if
        // some future caller gets it wrong, because an epoch that moves backwards
        // silently disarms every downstream check at once.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: refusing to move epoch backwards ({} -> {}) -- keeping {}", epoch_,
                   new_epoch, epoch_);
        return;
    }
    if (new_epoch == epoch_) {
        return;
    }

    const int32_t previous = epoch_;
    epoch_ = new_epoch;

    // Written before the new epoch is acted on, so a crash in the gap cannot bring
    // the node back believing it still owns a generation it has spent.
    if (!epoch_store_.store(epoch_)) {
        // Carry on in the new generation regardless. Stopping would take out a
        // matching engine that is otherwise healthy, and the in-memory epoch is
        // still correct for as long as this process lives. What is lost is the
        // guarantee across a restart, and that is worth an alert.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: epoch advanced {} -> {} but could not be written to {} -- a restart from here may reuse a spent generation", previous,
                   epoch_, epoch_store_.path());
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: epoch advanced {} -> {} (recorded)", previous, epoch_);
}

const char* MatchingEngineThread::me_role_name(MeRole role) {
    switch (role) {
        case MeRole::Unknown:
            return "UNKNOWN";
        case MeRole::Follower:
            return "FOLLOWER";
        case MeRole::Reconciling:
            return "RECONCILING";
        case MeRole::Leader:
            return "LEADER";
    }
    return "UNKNOWN";
}

void MatchingEngineThread::adopt_leader_role() {
    if (role_.role() == MeRole::Leader) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: adopting LEADER role (epoch={})", epoch_);
    report_role_change(role_.role(), MeRole::Leader);
    role_.became_leader();
    announce_role();

    // This instance is going to serve, so anything it caught up on and held back is owed to the
    // members who placed those orders.
    report_sending_.release_now_serving(epoch_);
}

void MatchingEngineThread::act_on(pubsub_itc_fw::PairLeaseAgent::Change change) {
    set_epoch(lease_agent_->highest_epoch());
    switch (change) {
        case pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader:
            // The lease settles the role. An instance that is not already leading catches up from
            // the WAL before it acts on it, whatever it has done so far.
            become_leader_when_current();
            static_cast<void>(bar_peer_if_settings_differ());
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::StoppedLeading:
            // Stops matching at once, whatever it was doing, and tells the sequencers so they stop
            // routing orders here.
            catch_up_.stop_awaiting_halt();
            enter_follower_state();
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::AgreedPeerLeads:
            if (role_.catching_up() && !role_.catching_up_to_lead()) {
                // Catching up at startup: follow once the catch-up ends.
                role_.note_peer_leader(peer_instance_id(), lease_agent_->highest_epoch());
            } else if (role_.role() == MeRole::Unknown) {
                static_cast<void>(follow_peer_claiming_leadership(peer_instance_id(), lease_agent_->highest_epoch()));
            }
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void MatchingEngineThread::EngineLeaseLinks::send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer = owner_.outbound_peer_conn_id_.is_valid() ? owner_.outbound_peer_conn_id_ : owner_.inbound_peer_conn_id_;
    if (peer.is_valid()) {
        owner_.send_pdu(peer, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
    }
}

void MatchingEngineThread::EngineLeaseLinks::send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) {
    // Both arbiters: only the active one answers, and which one that is may have changed.
    for (const pubsub_itc_fw::ConnectionID& conn : {owner_.arbiter_primary_conn_id_, owner_.arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            owner_.send_pdu(conn, pubsub_itc_fw::LeaseRequest::message_pdu_id, 0, request);
        }
    }
}

void MatchingEngineThread::EngineLeaseLinks::send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseGrant::message_pdu_id, 0, grant);
}

void MatchingEngineThread::EngineTestEventLinks::send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes,
                                                                 size_t size) {
    owner_.send_pdu_payload(connection, pdu_id, 0, bytes, size);
}

namespace {

// An engine role as a test event names it: an instance still becoming current neither leads nor follows yet.
pubsub_itc_fw::LeadershipRole leadership_role(MeRole role) {
    switch (role) {
        case MeRole::Leader:
            return pubsub_itc_fw::LeadershipRole::Leader;
        case MeRole::Follower:
            return pubsub_itc_fw::LeadershipRole::Follower;
        case MeRole::Unknown:
        case MeRole::Reconciling:
            break;
    }
    return pubsub_itc_fw::LeadershipRole::Unknown;
}

} // un-named namespace

void MatchingEngineThread::report_role_change(MeRole from, MeRole to) {
    if (test_events_.has_value()) {
        test_events_->role_changed("MatchingEngineThread", static_cast<int64_t>(config_.instance_id), leadership_role(from), leadership_role(to), epoch_);
    }
}

void MatchingEngineThread::report_catch_up(pubsub_itc_fw_app::CatchUpStage stage, int64_t through_seq_no, int64_t records_sent) {
    if (!test_events_.has_value()) {
        return;
    }
    pubsub_itc_fw_app::CatchUp event{};
    event.stage = stage;
    event.through_seq_no = through_seq_no;
    event.records_applied = catch_up_.records_seen();
    event.records_sent = records_sent;
    event.reports_sent = catch_up_.reports_sent();
    event.book_size = static_cast<int64_t>(order_book_.size());
    event.reports_left_out = catch_up_.reports_left_out();
    test_events_->send_event(pubsub_itc_fw_app::CatchUp::message_pdu_id, event);
}

void MatchingEngineThread::report_present_state() {
    if (!test_events_.has_value()) {
        return;
    }
    if (!region_unusable_reason_.empty()) {
        test_events_->send_event(pubsub_itc_fw_app::RegionUnusable::message_pdu_id, pubsub_itc_fw_app::RegionUnusable{region_unusable_reason_});
    }
    if (halt_reason_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::Halted::message_pdu_id, pubsub_itc_fw_app::Halted{*halt_reason_});
    }
    if (peer_settings_known_ && peer_settings_differ_) {
        report_settings_differ_from_peer();
    }
    if (died_applying_outcome_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::DiedApplying::message_pdu_id, pubsub_itc_fw_app::DiedApplying{died_applying_, *died_applying_outcome_});
    }
    if (role_.role() == MeRole::Reconciling) {
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::Started, catch_up_.position(), 0);
        return;
    }
    if (!role_.catch_up_completed()) {
        return;
    }
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Completed, catch_up_.position(), catch_up_.last_records_sent());
    if (role_.role() == MeRole::Unknown) {
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::CurrentAndWaiting, catch_up_.position(), catch_up_.last_records_sent());
    }
}

void MatchingEngineThread::report_order_outcome(const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                                                pubsub_itc_fw_app::OrderOutcomeKind outcome, ReportIsRepeat repeat, int64_t sequence_number,
                                                std::string_view orig_cl_ord_id) {
    if (!test_events_.has_value() || !config_.test_events.reports_orders_of(session.comp_id_view())) {
        return;
    }
    pubsub_itc_fw_app::OrderOutcome event{};
    event.comp_id = session.comp_id_view();
    event.cl_ord_id = cl_ord_id;
    event.orig_cl_ord_id = orig_cl_ord_id;
    event.outcome = outcome;
    event.during_catch_up = repeat == ReportIsRepeat::yes;
    event.seq_no = sequence_number;
    test_events_->send_event(pubsub_itc_fw_app::OrderOutcome::message_pdu_id, event);
}

void MatchingEngineThread::EngineLeaseLinks::send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) {
    owner_.send_pdu(conn_id, pubsub_itc_fw::LeaseRefusal::message_pdu_id, 0, refusal);
}

void MatchingEngineThread::handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseRequestView request{};
    if (!pubsub_itc_fw::decode(request, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode LeaseRequest -- dropping");
        return;
    }
    if (fix_common::group_of(request.pair_id) != pubsub_itc_fw_app::ComponentGroup::matching_engine || !lease_agent_.has_value()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: LeaseRequest for group={} -- dropping",
                   pubsub_itc_fw_app::to_string(fix_common::group_of(request.pair_id)));
        return;
    }
    act_on(lease_agent_->on_request(
        conn_id, request.candidate_instance_id, request.epoch, request.request_id,
        pubsub_itc_fw::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead},
        std::chrono::steady_clock::now()));
}

void MatchingEngineThread::handle_lease_grant(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseGrantView grant{};
    if (!pubsub_itc_fw::decode(grant, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode LeaseGrant -- dropping");
        return;
    }
    if (fix_common::group_of(grant.pair_id) != pubsub_itc_fw_app::ComponentGroup::matching_engine || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, std::chrono::steady_clock::now()));
}

void MatchingEngineThread::handle_lease_refusal(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::LeaseRefusalView refusal{};
    if (!pubsub_itc_fw::decode(refusal, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode LeaseRefusal -- dropping");
        return;
    }
    if (fix_common::group_of(refusal.pair_id) != pubsub_itc_fw_app::ComponentGroup::matching_engine || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, std::chrono::steady_clock::now()));
}

void MatchingEngineThread::enter_follower_state() {
    const MeRole previous = role_.role();
    role_.became_follower();
    // Logged here rather than at each caller, so no path into this state is silent. Support
    // reading a log after an incident wants every role CHANGE in it -- but a secondary that
    // starts as a follower and is then confirmed as one has not changed anything, and
    // "role now FOLLOWER (was FOLLOWER)" is noise dressed as an event.
    if (previous != MeRole::Follower) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: role now FOLLOWER (was {}, epoch={}) -- passive, not accepting orders",
                   me_role_name(previous), epoch_);
        report_role_change(previous, MeRole::Follower);
    }
    // Anything held back from a catch-up is not this instance's to send: it serves nobody, and
    // the leader answered those members from its own catch-up.
    report_sending_.discard("this instance is a follower and serves nobody");

    announce_role();
    if (ha_enabled_ && previous == MeRole::Leader) {
        // It was sent commands by order routing; from now on it follows the WAL.
        follow_the_wal_again("it stopped leading");
    }
}

bool MatchingEngineThread::keeps_its_reports() const {
    return role_.keeps_its_reports();
}

void MatchingEngineThread::keep_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t seq_no, const fix_common::SessionIdentity& session) {
    HeldEngineReport kept;
    kept.seq_no = seq_no;
    kept.session = session;
    // Sent, if ever, by an instance taking the lead after the leader that may have sent it already.
    kept.poss_resend = true;
    // Not encoded when there is no room for it: the part then only says that the store is full.
    if (report_comparison_.has_room()) {
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(pubsub_itc_fw_app::encode(report, nullptr, 0, bytes_written, bytes_needed));
        kept.payload.resize(bytes_needed);
        if (!pubsub_itc_fw_app::encode(report, kept.payload.data(), kept.payload.size(), bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "MatchingEngineThread: failed to encode a report to keep ({} bytes needed)",
                       bytes_needed);
            return;
        }
        kept.payload.resize(bytes_written);
    }
    report_comparison_.keep(std::move(kept));
}

void MatchingEngineThread::send_kept_reports_after(int64_t reported_through) {
    static_cast<void>(report_comparison_.take_reports_after(reported_through, [this](HeldEngineReport&& kept) { report_sending_.hold(std::move(kept)); }));
    report_sending_.send_held_on_the_connections_up(epoch_);
}

void MatchingEngineThread::ReportComparisonLinks::stop_leading_for_good(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason) {
    if (owner_.lease_agent_.has_value()) {
        owner_.lease_agent_->stop_asking_to_lead();
    }
    owner_.order_book_.mark_unusable();
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::FollowerCannotLead::message_pdu_id, pubsub_itc_fw_app::FollowerCannotLead{command, reason});
    }
}

void MatchingEngineThread::ReportComparisonLinks::difference_found() {
    owner_.follower_report_differences_counter_.increment();
}

void MatchingEngineThread::ReportComparisonLinks::kept_reports_sent(int64_t sent, int64_t reported_through) {
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::KeptReportsSent::message_pdu_id, pubsub_itc_fw_app::KeptReportsSent{sent, reported_through});
    }
}

bool MatchingEngineThread::following_the_wal() const {
    return role_.following_the_wal();
}

bool MatchingEngineThread::next_followed_record(int64_t seq_no) {
    if (catch_up_.next_followed_record(seq_no)) {
        return true;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineThread: following the WAL, record {} arrived where record {} was due -- a record was missed or sent again. Applying "
               "nothing more from this answer, and asking to be caught up again from the last command applied, {}",
               seq_no, catch_up_.next_followed(), state_machine_.highest_applied());
    follow_the_wal_again("a record arrived out of turn");
    return false;
}

void MatchingEngineThread::follow_the_wal_again(const char* why) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: following the WAL -- {}, so asking to be caught up again from the last command applied, {}, and to go on following", why,
               state_machine_.highest_applied());
    role_.start_catching_up_to_follow_again();
    catch_up_.stop_following();
    begin_reconciliation();
}

bool MatchingEngineThread::follow_peer_claiming_leadership(int64_t instance_id, int32_t announced_epoch) {
    if (announced_epoch < epoch_) {
        // A leader from a generation older than one this node has already seen. It
        // led once and has not learned that it stopped. Following it would put the
        // venue back into a generation it has left, so wait for the arbiter.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: peer (instance_id={}) claims leader at epoch {} but this node has seen epoch {} -- not following a stale leader",
                   instance_id, announced_epoch, epoch_);
        return false;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: peer (instance_id={}) leads at epoch {} -- this instance granted it a lease, and follows it", instance_id,
               announced_epoch);

    // Take the peer's generation rather than inventing one: this is deference, and
    // the generation being deferred to is the peer's.
    set_epoch(announced_epoch);
    enter_follower_state();
    return true;
}

void MatchingEngineThread::act_on_peer_leader_heard_while_reconciling() {
    // Consumed whether or not it is followed, so a claim heard once cannot be applied twice and cannot outlive the
    // catch-up it arrived during.
    const std::optional<EngineRole::PeerLeader> noted = role_.take_peer_leader_note();
    if (!noted.has_value()) {
        return;
    }
    // Checked against the epoch as it stands now, not as it stood when the announcement arrived: reconciliation can have
    // advanced this instance's own generation in between.
    static_cast<void>(follow_peer_claiming_leadership(noted->instance_id, noted->epoch));
}

void MatchingEngineThread::announce_role() {
    // Reconciling counts as follower to the outside world: this instance has been told it
    // will lead but cannot serve until its book is caught up, and orders sent meanwhile
    // would be dropped. It announces LEADER from adopt_leader_role, once it can.
    if (role_.role() != MeRole::Leader && role_.role() != MeRole::Follower) {
        return;
    }
    pubsub_itc_fw_app::RoleAnnouncement announcement{};
    announcement.instance_id = static_cast<int64_t>(config_.instance_id);
    announcement.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    announcement.current_role = role_.role() == MeRole::Leader ? pubsub_itc_fw_app::Role::leader : pubsub_itc_fw_app::Role::follower;
    announcement.epoch = epoch_;

    // The peer gets this too, not just the sequencers. An instance starting beside
    // an established leader can then adopt follower on the strength of the leader
    // saying so, instead of waiting on arbitration that a recently started arbiter
    // will decline. Both connections to the peer are held permanently, so whichever
    // one is up carries it.
    for (const pubsub_itc_fw::ConnectionID& conn :
         {report_sending_.connection(EngineReportSending::Sequencer::primary), report_sending_.connection(EngineReportSending::Sequencer::secondary),
          outbound_peer_conn_id_, inbound_peer_conn_id_}) {
        if (conn.is_valid()) {
            send_pdu(conn, pubsub_itc_fw_app::RoleAnnouncement::message_pdu_id, 0, announcement);
        }
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: announced role {} at epoch {} to the sequencers and the peer",
               pubsub_itc_fw_app::to_string(announcement.current_role), epoch_);
}

// WAL reconciliation (Slice D)

void MatchingEngineThread::recover_open_orders(bool region_existed) {
    if (catch_up_.region_was_unusable()) {
        // The region this process inherited could not be used, so this one is new, but the engine is not: it held orders,
        // and the venue must say what became of them. It asks for every record from the first, so that the catch-up
        // rebuilds the book from the WAL (R-0123); account_for_unusable_region decides what follows.
        order_book_.warm();
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: the region it inherited could not be used -- asking for every record of the WAL from the first, to rebuild the "
                       "book from it (R-0123)");
        return;
    }
    if (!region_existed) {
        // Nothing was left behind, so there is nothing to read and every page of the region is
        // a hole. Touching them now costs about half a millisecond and means no order pays a
        // fault later, which is R-0121.
        order_book_.warm();
        // And no position either, which is a different thing from a position of zero. Zero
        // would mean "I have applied nothing of a log I am part of", and the venue would send
        // this instance the whole of its retained record -- every order of the day rather than
        // the few taken while nothing was running. This instance is not behind; it is new, and
        // it says so and is placed at the venue's current position instead.
        catch_up_.no_region_found();
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: no order book region was left behind -- starting with an empty book and no position of its own");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const OrderBook::Recovery found = order_book_.recover();
    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // The scan above read every page, so there is nothing left for warm() to do.

    // Where the tail starts. The region is current to what it published and no further, so
    // everything the sequencer holds after that has still to be applied -- which is the
    // existing reconciliation path, asked to start here instead of at zero.
    catch_up_.region_found_current_to(found.published);
    // Everything at or below it has been applied, so that a void this engine is told of before its catch-up is judged
    // against the commands it applied before it stopped (MatchingEngineStateMachine::learn_void).
    state_machine_.note_caught_up_to(found.published);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: recovered {} open order(s) from the region at seq_no={} in {} ms ({} unfinished record(s) discarded)", found.orders,
               found.published, elapsed_ms, found.discarded);

    // A command recorded as being applied, above the published position, is one the process before this one died
    // applying. One at or below it was applied and published, and the process died before recording that it had finished:
    // it is not applied again, so there is nothing to ask.
    const int64_t applying = order_book_.applying_when_opened();
    if (applying > found.published) {
        died_applying_ = applying;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the process before this one died applying command seq={} -- asking whether the peer engine applied it, and if "
                   "not, for it to be voided, before catching up (docs/availability/voided_commands.md, section 4)",
                   applying);
    } else if (applying != 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: the process before this one died after applying command seq={} and publishing it, before recording that it had "
                   "finished -- the command is not applied again, so nothing is asked",
                   applying);
    }

    if (order_book_.halted()) {
        // Set by a command the engine applied before it stopped, and kept in the region with the orders.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: the region records that trading is halted -- the venue refuses every new order until a person lifts the halt "
                       "(R-0023)");
    }
}

void MatchingEngineThread::become_leader_when_current() {
    if (role_.role() == MeRole::Leader) {
        // Already serving: it has applied every order as it arrived, and catching up now would
        // make the live orders arriving meanwhile read as replay.
        return;
    }
    // Every other instance catches up first, whatever it has done before. One that caught up when
    // the first sequencer connected, and has waited since to learn its role, was current only at
    // that moment: a sequencer sends orders only to the engine that leads, so every order accepted
    // while it waited reached it only through the WAL. Leading without catching up would lose
    // each of them, and tell no member (BUG-0198). A catch-up to follow again that is under way is replaced: its answer is
    // not waited for (BUG-0218).
    role_.start_catching_up_to_lead();
    begin_reconciliation();
}

void MatchingEngineThread::begin_reconciliation() {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: entering RECONCILING (last_replicated_seq_no={}, book_size={})",
               catch_up_.position(), order_book_.size());
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Started, catch_up_.position(), 0);
    if (catch_up_.begin(catch_up_context())) {
        role_.request_sent(role_.catching_up_to_lead() || !ha_enabled_);
    }
}

EngineCatchUp::RequestContext MatchingEngineThread::catch_up_context() const {
    EngineCatchUp::RequestContext context{};
    context.may_ask = died_applying_question_ == DiedApplyingQuestion::none;
    context.died_applying = died_applying_;
    context.to_lead = role_.catching_up_to_lead();
    context.highest_applied = state_machine_.highest_applied();
    return context;
}

void MatchingEngineThread::send_follow_request() {
    if (catch_up_.send_request(catch_up_context())) {
        // Whether the latest request asked to follow: with high availability on, every request not to lead does.
        role_.request_sent(role_.catching_up_to_lead() || !ha_enabled_);
    }
}

void MatchingEngineThread::ask_on_new_sequencer_connection(const pubsub_itc_fw::ConnectionID& id) {
    if (!catch_up_.request_sent()) {
        begin_reconciliation();
        return;
    }
    if (following_the_wal()) {
        // A new connection to a sequencer while following: if it is the leading sequencer's, the connection that carried
        // the records is gone, and what it was sending with it.
        follow_the_wal_again("a sequencer connected");
        return;
    }
    catch_up_.send_request_on(id, catch_up_context());
}

void MatchingEngineThread::handle_follow_declined(const pubsub_itc_fw::EventMessage& message) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: the sequencer on connection {} does not lead and declined to catch this instance up -- it says when it leads",
               message.connection_id().get_value());
}

void MatchingEngineThread::handle_leader_changed(const pubsub_itc_fw::EventMessage& message) {
    if (following_the_wal()) {
        follow_the_wal_again("another sequencer leads");
        return;
    }
    if (role_.role() != MeRole::Reconciling) {
        return; // caught up already: nothing to ask for
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: the sequencer on connection {} now leads -- asking again to be caught up",
               message.connection_id().get_value());
    send_follow_request();
}

void MatchingEngineThread::send_engine_settings() {
    if (!outbound_peer_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::EngineSettings settings{};
    settings.instance_id = static_cast<int64_t>(config_.instance_id);
    settings.order_book_region_capacity = static_cast<int64_t>(order_book_.region_capacity());
    settings.identifier_secret_fingerprint = report_identifiers_.secret_fingerprint();
    send_pdu(outbound_peer_conn_id_, pubsub_itc_fw_app::EngineSettings::message_pdu_id, 0, settings);
}

void MatchingEngineThread::handle_engine_settings(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::EngineSettingsView settings{};
    if (!pubsub_itc_fw_app::decode(settings, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode EngineSettings -- dropping");
        return;
    }
    const auto own_region_capacity = static_cast<int64_t>(order_book_.region_capacity());
    const bool differed = peer_settings_known_ && peer_settings_differ_;
    peer_settings_known_ = true;
    peer_region_capacity_ = settings.order_book_region_capacity;
    peer_secret_fingerprint_ = settings.identifier_secret_fingerprint;
    const bool secrets_differ = peer_secret_fingerprint_ != report_identifiers_.secret_fingerprint();
    peer_settings_differ_ = peer_region_capacity_ != own_region_capacity || secrets_differ;
    if (peer_region_capacity_ != own_region_capacity) {
        // The open-order region's capacity decides when an order is refused because the venue holds as many as it can,
        // so two engines with different capacities apply the same commands differently once one of them is full.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the peer, instance {}, holds up to {} open order(s) and this instance up to {} -- the two would apply the same "
                   "commands differently, so whichever of them leads says that the other may not lead",
                   settings.instance_id, peer_region_capacity_, own_region_capacity);
    }
    if (secrets_differ) {
        // The secret the identifiers' keys are derived from: two engines with different secrets give the same report
        // different ExecIDs, so a member would be told of one event under two names after a failover.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the peer, instance {}, holds a different identifier secret from this instance's ({}) -- the two would name the "
                   "same reports differently, so whichever of them leads says that the other may not lead. Copy the one secret file to both",
                   settings.instance_id, config_.identifier_secret_file);
    }
    if (!peer_settings_differ_ && differed) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: the peer, instance {}, is now configured as this instance is (up to {} open order(s))", settings.instance_id,
                   own_region_capacity);
    }
    // Reported whether or not this instance leads, so that the test learns the follower knows of the difference too,
    // and only once: bar_peer_if_settings_differ reports it when it bars the peer.
    if (!bar_peer_if_settings_differ() && peer_settings_differ_) {
        report_settings_differ_from_peer();
    }
}

void MatchingEngineThread::report_settings_differ_from_peer() {
    if (!test_events_.has_value()) {
        return;
    }
    const bool barred_peer = lease_agent_.has_value() && lease_agent_->leading() && !lease_agent_->says_peer_may_lead();
    test_events_->send_event(pubsub_itc_fw_app::SettingsDifferFromPeer::message_pdu_id,
                             pubsub_itc_fw_app::SettingsDifferFromPeer{peer_instance_id(), static_cast<int64_t>(order_book_.region_capacity()),
                                                                       peer_region_capacity_, barred_peer,
                                                                       peer_secret_fingerprint_ != report_identifiers_.secret_fingerprint()});
}

bool MatchingEngineThread::bar_peer_if_settings_differ() {
    if (!lease_agent_.has_value() || !lease_agent_->leading() || !peer_settings_known_) {
        return false;
    }
    // Already saying what it should, so there is nothing to record or send.
    if (peer_settings_differ_ != lease_agent_->says_peer_may_lead()) {
        return false;
    }
    // Recorded by a majority of the voters, as rule 11's statement is, so that the peer is refused a lease even after
    // this instance has gone; and lifted once the peer says it is configured as this instance is.
    if (peer_settings_differ_) {
        lease_agent_->say_peer_may_not_lead(std::chrono::steady_clock::now());
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: leading, and saying that the peer, instance {}, may not lead -- it holds up to {} open order(s) and this instance "
                   "up to {}, and its identifier secret is {} this instance's. Trading continues here; if this instance stops, no engine leads until "
                   "the two are configured alike",
                   peer_instance_id(), peer_region_capacity_, order_book_.region_capacity(),
                   peer_secret_fingerprint_ == report_identifiers_.secret_fingerprint() ? "the same as" : "different from");
        report_settings_differ_from_peer();
        return true;
    }
    lease_agent_->say_peer_may_lead(std::chrono::steady_clock::now());
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: leading, and saying that the peer, instance {}, may lead again -- it is configured as this instance is",
               peer_instance_id());
    return false;
}

void MatchingEngineThread::handle_engine_position_query(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::EnginePositionQueryView query{};
    if (!pubsub_itc_fw_app::decode(query, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode EnginePositionQuery -- dropping");
        return;
    }

    // Answered only by an engine that acts on the orders it is sent, the same test the order path
    // applies. An engine that is still catching up as part of a promotion catches up from the
    // sequencer that asks, so its answer would be out of date by the time it finished; the
    // sequencer asks again until it gets one.
    const bool acting_on_orders = !ha_enabled_ || role_.role() == MeRole::Leader;
    if (!acting_on_orders) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: EnginePositionQuery {} on connection {} -- not acting on orders (state={}), not answering", query.request_id,
                   message.connection_id().get_value(), static_cast<int>(role_.role()));
        return;
    }

    pubsub_itc_fw_app::EnginePosition answer{};
    answer.request_id = query.request_id;
    answer.highest_applied = state_machine_.highest_applied();
    send_pdu(message.connection_id(), pubsub_itc_fw_app::EnginePosition::message_pdu_id, 0, answer);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: EnginePositionQuery {} on connection {} -- the highest order or cancel acted on is seq_no={}", query.request_id,
               message.connection_id().get_value(), state_machine_.highest_applied());
}

bool MatchingEngineThread::from_an_earlier_answer(const pubsub_itc_fw::ConnectionID& connection) const {
    // While catching up, only the answer to the latest request is applied. A record that arrives on another connection,
    // or before that answer has started, answers an earlier request: the leading sequencer stops answering an earlier
    // request on a connection before it answers a later one, but what it had already sent still arrives. Every record
    // it holds is in the latest answer, which starts from the last command this instance applied (BUG-0212).
    return role_.role() == MeRole::Reconciling && catch_up_.from_an_earlier_answer(connection);
}

void MatchingEngineThread::handle_follow_started(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::WalFollowStartedView started{};
    if (!pubsub_itc_fw::decode(started, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode WalFollowStarted -- dropping");
        return;
    }
    if (!catch_up_.started(message.connection_id(), started, role_.role() == MeRole::Reconciling)) {
        return;
    }
    if (role_.latest_request_asked_to_follow()) {
        report_comparison_.start_keeping_after(started.after_seq_no);
    }
    if (role_.catching_up_to_lead()) {
        send_kept_reports_after(catch_up_.reports_in_wal_through());
    }
}

void MatchingEngineThread::handle_caught_up(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw::WalFollowCaughtUpView ack{};

    if (!pubsub_itc_fw::decode(ack, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode WalFollowCaughtUp -- dropping");
        return;
    }

    // Reconcile on the first ack only. We fan the request out to every
    // sequencer order connection, so a straggler ack (e.g. from a sequencer that
    // just won an election) can still arrive after we have promoted; ignore it
    // rather than re-running cancel-on-failover.
    if (role_.role() != MeRole::Reconciling) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: already promoted (state={}) -- ignoring a duplicate caught-up message",
                   static_cast<int>(role_.role()));
        return;
    }
    switch (catch_up_.caught_up(message.connection_id(), ack, order_book_.size())) {
        case EngineCatchUp::Answer::other_request:
            return;
        case EngineCatchUp::Answer::incomplete:
            report_catch_up(pubsub_itc_fw_app::CatchUpStage::Incomplete, ack.last_seq_no, ack.records_sent);
            return;
        case EngineCatchUp::Answer::complete:
            break;
    }
    state_machine_.note_caught_up_to(ack.last_seq_no);
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Completed, ack.last_seq_no, ack.records_sent);

    if (role_.situation() == EngineRole::Situation::catching_up_to_follow_again) {
        static_cast<void>(role_.catch_up_completed_now());
        catch_up_.follow_from(ack.last_seq_no + 1);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: caught up again at seq_no={} with {} order(s) on the book -- following the WAL from record {}, as {}",
                   ack.last_seq_no, order_book_.size(), catch_up_.next_followed(), me_role_name(role_.role()));
        return;
    }

    // The book is now assembled: what the region vouched for, plus every record the sequencer held after it. Every order
    // on it is kept, whether this instance is starting or taking the lead: an engine that followed the WAL applied every
    // command the leader applied, so its book is the one the members were told of (R-0018, R-0073;
    // docs/availability/both_engines_apply_the_wal.md, section 5).
    //
    // Where the region could not be used, R-0123 decides first: the book the catch-up rebuilt is kept if the WAL
    // reached the first command of the day, and otherwise trading halts.
    if (!book_can_be_vouched_for()) {
        catch_up_.account_for_unusable_region(ack, !ha_enabled_ || role_.catching_up_to_lead(), order_book_.size());
    }
    if (!book_can_be_vouched_for()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "MatchingEngineThread: holding {} order(s) it cannot vouch for", order_book_.size());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: caught up -- keeping the {} open order(s) on the book (R-0018)",
                   order_book_.size());
    }

    // Current, as the role reports it, until a catch-up to lead adopts the leader role below.
    const EngineRole::Completed completed = role_.catch_up_completed_now();

    if (completed != EngineRole::Completed::to_lead) {
        if (role_.latest_request_asked_to_follow()) {
            catch_up_.follow_from(ack.last_seq_no + 1);
        }
        // Current, and that is all. This catch-up was done at startup so that nothing is
        // served on a stale book; whether this instance may act is a separate question, and is
        // answered by whether a majority grants it a lease. Leading here would have every
        // instance that manages to catch up decide for itself that it leads, which is the two
        // leaders the lease rules exist to prevent.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: caught up at seq_no={} with {} order(s) on the book -- current, and waiting to be told what it may do",
                   ack.last_seq_no, order_book_.size());
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::CurrentAndWaiting, ack.last_seq_no, ack.records_sent);
        // A leader that announced itself while this catch-up was running said so on the only
        // occasion it had. Acting on it here is what keeps the startup arbitration deadline from
        // degrading this instance into a second leader while the first is alive and well.
        // Skipped where trading is halted: that decision is not this announcement's to reopen.
        if (!state_machine_.halted()) {
            act_on_peer_leader_heard_while_reconciling();
        }
        return;
    }

    // Transition to LEADER -- normal processing begins.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: reconciliation complete at seq_no={} with {} order(s) on the book -- resuming as leader", ack.last_seq_no,
               order_book_.size());
    resumed_through_ = ack.last_seq_no;
    report_resumed_as_leader();
    adopt_leader_role();
}

bool MatchingEngineThread::book_can_be_vouched_for() const {
    // The region is what vouches for the book: it holds the orders themselves, and what it
    // published says how far they can be trusted. Where the one this process inherited could
    // not be read, the engine holds only what the sequencer's record supplied, which is a
    // different thing and must not be traded on as though it were the same.
    return order_book_.is_open() && !catch_up_.region_was_unusable();
}

void MatchingEngineThread::CatchUpLinks::send_follow_request(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequest& request) {
    owner_.send_pdu(connection, pubsub_itc_fw::WalFollowRequest::message_pdu_id, 0, request);
}

void MatchingEngineThread::CatchUpLinks::start_retry_timer() {
    owner_.reconciliation_timer_id_ = owner_.start_recurring_timer(std::chrono::seconds(owner_.config_.catch_up_retry_seconds));
}

void MatchingEngineThread::CatchUpLinks::stop_retry_timer() {
    owner_.cancel_timer(owner_.reconciliation_timer_id_);
    owner_.reconciliation_timer_id_ = pubsub_itc_fw::TimerID{};
}

void MatchingEngineThread::CatchUpLinks::request_halt() {
    owner_.request_halt();
}

void MatchingEngineThread::CatchUpLinks::start_halt_timer() {
    if (!owner_.halt_request_timer_id_.is_valid()) {
        owner_.halt_request_timer_id_ = owner_.start_recurring_timer(halt_request_interval);
    }
}

void MatchingEngineThread::CatchUpLinks::stop_halt_timer() {
    owner_.cancel_timer(owner_.halt_request_timer_id_);
    owner_.halt_request_timer_id_ = pubsub_itc_fw::TimerID{};
}

void MatchingEngineThread::CatchUpLinks::region_rebuilt(int64_t through) {
    if (owner_.test_events_.has_value()) {
        owner_.test_events_->send_event(pubsub_itc_fw_app::RegionRebuilt::message_pdu_id,
                                        pubsub_itc_fw_app::RegionRebuilt{through, static_cast<int64_t>(owner_.order_book_.size())});
    }
}

void MatchingEngineThread::request_halt() {
    pubsub_itc_fw_app::HaltRequest request{};
    request.instance_id = static_cast<int64_t>(config_.instance_id);
    request.epoch = epoch_;
    request.reason = pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook;
    // To every sequencer order connection, as a catch-up request is: only the leading sequencer writes the event.
    for (const auto& conn_id : sequencer_order_conn_ids_) {
        send_pdu(conn_id, pubsub_itc_fw_app::HaltRequest::message_pdu_id, 0, request);
    }
}

void MatchingEngineThread::apply_cancel_book_command(const pubsub_itc_fw_app::CancelBookView& command, int64_t sequence_number, int64_t transact_time,
                                                     int32_t trading_date, CommandSource source) {
    const int64_t orders_placed_through = command.orders_placed_through;
    const MatchingEngineStateMachine::BookCancelledByCommand result =
        state_machine_.apply_cancel_book(orders_placed_through, sequence_number, transact_time, trading_date, source);
    if (!result.applied) {
        return;
    }
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::BookCancelled::message_pdu_id,
                                 pubsub_itc_fw_app::BookCancelled{result.open_orders, result.reports_sent, true, result.orders_kept});
    }
    // The orders are not stale because they are old. They are stale because the members that placed them were locked out
    // of them while the market moved: no engine could trade, so they could not cancel.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineThread: the leading sequencer wrote at seq={} that no matching engine could be trading within the longest recovery the "
               "venue does by itself -- {} order(s) cancelled and their members told (R-0117)",
               sequence_number, result.reports_sent);
}

void MatchingEngineThread::apply_technical_event_command(const pubsub_itc_fw_app::TechnicalEventView& event, int64_t sequence_number, int32_t trading_date,
                                                         CommandSource source) {
    const MatchingEngineStateMachine::TechnicalEventApplied result = state_machine_.apply_technical_event(event, sequence_number, trading_date, source);
    if (!result.applied || !result.halt_changed) {
        return;
    }
    if (event.halted) {
        catch_up_.stop_awaiting_halt();
        std::optional<pubsub_itc_fw_app::HaltReason> reason;
        switch (event.reason) {
            case pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook:
                reason = pubsub_itc_fw_app::HaltReason::CannotAccountForBook;
                break;
            case pubsub_itc_fw_app::TechnicalEventReason::RecoveryTooLong:
                reason = pubsub_itc_fw_app::HaltReason::RecoveryTooLong;
                break;
            case pubsub_itc_fw_app::TechnicalEventReason::DeclaredByOperator:
                reason = pubsub_itc_fw_app::HaltReason::DeclaredByOperator;
                break;
            default:
                break;
        }
        if (reason.has_value()) {
            halt_reason_ = reason;
            if (test_events_.has_value()) {
                test_events_->send_event(pubsub_itc_fw_app::Halted::message_pdu_id, pubsub_itc_fw_app::Halted{*halt_reason_});
            }
        }
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: trading is halted -- the venue is not accepting orders and a person must lift this (R-0023)");
    } else {
        halt_reason_.reset();
        if (test_events_.has_value()) {
            test_events_->send_event(pubsub_itc_fw_app::HaltLifted::message_pdu_id, pubsub_itc_fw_app::HaltLifted{sequence_number});
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: the halt is lifted at seq={} ({}) -- new orders are acted on again",
                   sequence_number, pubsub_itc_fw_app::to_string(event.reason));
    }
}

void MatchingEngineThread::handle_voided_commands(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::VoidedCommandsView view{};
    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "MatchingEngineThread: failed to decode VoidedCommands -- dropping");
        return;
    }
    for (const pubsub_itc_fw_app::VoidedCommandView& voided : view.voids) {
        const MatchingEngineStateMachine::VoidLearned learned = state_machine_.learn_void(
            fix_common::VoidedRange{voided.void_seq_no, voided.first_seq_no, voided.last_seq_no, voided.new_orders_only, voided.reason});
        if (learned == MatchingEngineStateMachine::VoidLearned::after_applying_some) {
            report_comparison_.refuse_to_lead(voided.void_seq_no, pubsub_itc_fw_app::FollowerCannotLeadReason::AppliedVoidedCommand,
                                              "it applied a command before it was told that a later command in the WAL voided it");
        }
    }
    if (died_applying_question_ == DiedApplyingQuestion::asking_sequencers &&
        state_machine_.voids().voiding(died_applying_, fix_common::VoidedKind::cancel) != nullptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: command seq={}, which the process before this one died applying, is voided -- it is answered with a rejection, "
                   "and this instance asks to be caught up",
                   died_applying_);
        settle_died_applying(pubsub_itc_fw_app::DiedApplyingOutcome::Voided);
    }
}

void MatchingEngineThread::apply_void_commands_command(const pubsub_itc_fw_app::VoidCommandsView& command, int64_t sequence_number, CommandSource source) {
    const MatchingEngineStateMachine::VoidCommandsApplied result = state_machine_.apply_void_commands(command, sequence_number, source);
    if (result.learned_too_late) {
        report_comparison_.refuse_to_lead(sequence_number, pubsub_itc_fw_app::FollowerCannotLeadReason::AppliedVoidedCommand,
                                          "it reached a void without having been told of it, after applying the commands it names");
    }
}

void MatchingEngineThread::begin_died_applying_question() {
    if (died_applying_ == 0) {
        return;
    }
    if (!ha_enabled_) {
        ask_for_void("high availability is off, so there is no peer engine to ask");
        return;
    }
    died_applying_question_ = DiedApplyingQuestion::asking_peer;
    peer_answer_timer_id_ = start_one_off_timer(config_.lease.period);
    ask_peer_whether_applied();
}

void MatchingEngineThread::ask_peer_whether_applied() {
    if (died_applying_question_ != DiedApplyingQuestion::asking_peer || peer_asked_whether_applied_) {
        return;
    }
    const pubsub_itc_fw::ConnectionID peer = outbound_peer_conn_id_.is_valid() ? outbound_peer_conn_id_ : inbound_peer_conn_id_;
    if (!peer.is_valid()) {
        return; // asked when a connection to the peer is made
    }
    send_pdu(peer, pubsub_itc_fw_app::AppliedThroughQuery::message_pdu_id, 0,
             pubsub_itc_fw_app::AppliedThroughQuery{static_cast<int64_t>(config_.instance_id)});
    peer_asked_whether_applied_ = true;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: asked the peer engine on connection {} which commands it has applied, to learn whether command seq={} is what killed "
               "the process before this one",
               peer.get_value(), died_applying_);
}

void MatchingEngineThread::handle_applied_through_query(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::AppliedThroughQueryView query{};
    if (!pubsub_itc_fw_app::decode(query, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode AppliedThroughQuery -- dropping");
        return;
    }
    const pubsub_itc_fw_app::AppliedThrough answer{static_cast<int64_t>(config_.instance_id), state_machine_.highest_applied()};
    send_pdu(message.connection_id(), pubsub_itc_fw_app::AppliedThrough::message_pdu_id, 0, answer);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: the peer engine, instance {}, asked which commands this one has applied -- told it through seq={}", query.instance_id,
               answer.highest_applied);
}

void MatchingEngineThread::handle_applied_through(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::AppliedThroughView answer{};
    if (!pubsub_itc_fw_app::decode(answer, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode AppliedThrough -- dropping");
        return;
    }
    if (died_applying_question_ != DiedApplyingQuestion::asking_peer) {
        return; // the lease period ran out first, and the sequencers have been asked
    }
    if (answer.highest_applied < died_applying_) {
        ask_for_void(answer.highest_applied == died_applying_ - 1 ? "the peer engine has applied every command before it and not it"
                                                                  : "the peer engine has not applied it");
        return;
    }
    // The two engines apply the same commands in the same way, so the command did not kill the process before this one:
    // the peer applied it and lived.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
               "MatchingEngineThread: the peer engine has applied through seq={}, including command seq={}, which the process before this one died "
               "applying -- so the command did not cause the death, and it is applied as usual",
               answer.highest_applied, died_applying_);
    settle_died_applying(pubsub_itc_fw_app::DiedApplyingOutcome::PeerAppliedIt);
}

void MatchingEngineThread::ask_for_void(const char* why) {
    died_applying_question_ = DiedApplyingQuestion::asking_sequencers;
    if (peer_answer_timer_id_.is_valid()) {
        cancel_timer(peer_answer_timer_id_);
        peer_answer_timer_id_ = pubsub_itc_fw::TimerID{};
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineThread: asking the leading sequencer to void command seq={}, which the process before this one died applying, because {} "
               "(docs/availability/voided_commands.md, section 4.2)",
               died_applying_, why);
    send_void_request();
    void_request_timer_id_ = start_recurring_timer(void_request_interval);
}

void MatchingEngineThread::send_void_request() {
    pubsub_itc_fw_app::VoidRequest request{};
    request.instance_id = static_cast<int64_t>(config_.instance_id);
    request.epoch = epoch_;
    request.seq_no = died_applying_;
    // To every sequencer order connection, as a halt request is: only the leading sequencer acts on it.
    for (const auto& conn_id : sequencer_order_conn_ids_) {
        send_pdu(conn_id, pubsub_itc_fw_app::VoidRequest::message_pdu_id, 0, request);
    }
}

void MatchingEngineThread::handle_void_declined(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::VoidDeclinedView declined{};
    if (!pubsub_itc_fw_app::decode(declined, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode VoidDeclined -- dropping");
        return;
    }
    if (died_applying_question_ != DiedApplyingQuestion::asking_sequencers || declined.seq_no != died_applying_) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineThread: the leading sequencer will not void command seq={}, which the process before this one died applying ({}) -- it is "
               "applied as usual, and if it kills this process too, the supervisor does not restart it again",
               died_applying_, pubsub_itc_fw_app::to_string(declined.reason));
    settle_died_applying(pubsub_itc_fw_app::DiedApplyingOutcome::Declined);
}

void MatchingEngineThread::settle_died_applying(pubsub_itc_fw_app::DiedApplyingOutcome outcome) {
    died_applying_question_ = DiedApplyingQuestion::none;
    died_applying_outcome_ = outcome;
    for (pubsub_itc_fw::TimerID* timer : {&peer_answer_timer_id_, &void_request_timer_id_}) {
        if (timer->is_valid()) {
            cancel_timer(*timer);
            *timer = pubsub_itc_fw::TimerID{};
        }
    }
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::DiedApplying::message_pdu_id, pubsub_itc_fw_app::DiedApplying{died_applying_, outcome});
    }
    // A catch-up begun while the question was open asked for nothing; it is asked for now.
    if (role_.role() == MeRole::Reconciling && !catch_up_.request_sent() && !sequencer_order_conn_ids_.empty()) {
        send_follow_request();
    }
}

void MatchingEngineThread::report_resumed_as_leader() {
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::ResumedAsLeader::message_pdu_id,
                                 pubsub_itc_fw_app::ResumedAsLeader{resumed_through_, static_cast<int64_t>(order_book_.size())});
    }
}

} // namespaces
