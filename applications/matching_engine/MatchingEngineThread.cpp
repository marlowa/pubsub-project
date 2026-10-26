// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

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
    , cancel_open_orders_on_promotion_(config.order_book_open_orders_on_promotion == "cancel")
    , sequencer_er_conn_id_{}
    , sequencer_er_secondary_conn_id_{}
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
        region_was_unusable_ = true;
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

    // HA classification. A non-HA ME is a plain single instance (Unknown state,
    // full processing). The HA primary begins Unknown and adopts Leader once it
    // has connected to the arbiter (holding its lease). The HA secondary begins
    // as a passive Follower.
    if (ha_enabled_) {
        ha_role_state_ = is_primary_ ? MeRole::Unknown : MeRole::Follower;
        // A secondary used to adopt FOLLOWER without saying so, which left the most common
        // starting state of the pair as the one thing the log did not record. Someone reading
        // a log from a machine that has just come up should be able to see what it thinks it
        // is before anything else happens.
        // Recover the leadership generation before anything can elect. Without this
        // the node starts at zero, and a pair restarted together would agree on a
        // generation the venue has already used -- see pubsub_itc_fw::EpochStore.
        epoch_ = epoch_store_.load();

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: HA enabled, starting as {} (instance_id={}, configured {}, epoch={})",
                   me_role_name(ha_role_state_), config_.instance_id, is_primary_ ? "primary" : "secondary", epoch_);
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
        // Both roles dial, and both listen. Which connection carries book updates follows the
        // role, because the leader is the sender; holding both permanently means a role
        // change costs no connection work at the moment the venue can least afford it.
        connect_to_service("me_peer_replication");
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
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (test_events_.has_value() && test_events_->on_connection_established(id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: connection {} to the test's event listener established",
                   id.get_value());
        // A role taken, or a catch-up begun or finished, before the connection was made is reported now, so that the test
        // learns it whenever it connects.
        report_present_state();
        if (ha_role_state_ == MeRole::Leader || ha_role_state_ == MeRole::Follower) {
            report_role_change(MeRole::Unknown, ha_role_state_);
        }
        return;
    }
    const std::string& svc = id.service_name();
    const std::string order_inbound_svc = "inbound:" + std::to_string(config_.listen_port);
    const std::string replication_inbound_svc = "inbound:" + std::to_string(config_.replication_listen_port);

    if (svc == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: primary sequencer ER connection {} established", id.get_value());
        announce_role();
        release_reports_held_for_a_connection();
    } else if (svc == "sequencer_er_secondary") {
        sequencer_er_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
        announce_role();
        release_reports_held_for_a_connection();
    } else if (svc == "me_peer_replication") {
        // Our dial to the peer's replication listener. Used to send book updates while this
        // instance leads, and simply held open while it follows.
        outbound_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: ME-secondary replication connection {} established", id.get_value());
        send_engine_settings();
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
    } else if (ha_enabled_ && svc == replication_inbound_svc) {
        // The peer's dial to us. This is the channel we receive book updates on while we
        // follow, and losing it is how we learn the leader has gone -- which is why both
        // instances must have one, not only the configured secondary.
        inbound_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: ME-primary replication connection {} established -- replica book will be updated", id.get_value());
        // Tell the peer what this instance is as soon as there is a link to tell it
        // on. An instance that has just restarted is sitting in Unknown waiting for
        // arbitration, and a leader saying so here lets it settle immediately rather
        // than waiting on an arbiter that may decline while it is still learning.
        announce_role();
    } else if (!is_primary_ && ha_enabled_ && svc == order_inbound_svc) {
        // Secondary: inbound order connection from the sequencer. While in FOLLOWER
        // mode this is idle (order PDUs are discarded). If we are already RECONCILING
        // when the sequencer connects, begin WAL catch-up immediately.
        sequencer_order_conn_ids_.insert(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer order connection {} established (state={})",
                   id.get_value(), static_cast<int>(ha_role_state_));
        if (ha_role_state_ == MeRole::Reconciling) {
            ask_on_new_sequencer_connection(id);
        }
    } else {
        // Primary (or non-HA) inbound order connection from the sequencer.
        sequencer_order_conn_ids_.insert(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound sequencer order connection {} established", id.get_value());
        if (ha_role_state_ == MeRole::Reconciling) {
            // Either reconciliation was begun before any sequencer had dialled in, so the request
            // could not be sent, and sending it now is what keeps that from stranding the venue
            // with no engine leading -- the outage BUG-0043 recorded; or it was sent on another
            // connection, and only this one is still to be asked.
            ask_on_new_sequencer_connection(id);
        } else if (!has_reconciled_ && ha_role_state_ != MeRole::Follower) {
            // The catch-up belongs here, at the moment this instance first has somewhere to ask
            // and before it has served anything, rather than later when a role is settled. An
            // instance that starts and works for fifteen seconds while an arbiter makes up its
            // mind has already applied those orders; it is the orders taken BEFORE it started
            // that it has not seen, and this is the only moment it can ask about them without
            // mistaking live traffic for replay.
            //
            // A passive follower is excluded: it catches up when it is promoted, from the
            // replica it has been maintaining, which is a different position and a different
            // moment.
            //
            // An order the sequencer deferred while no engine was running is in the venue's
            // record and in no engine's book, so this ask is the only thing that recovers it.
            // See BUG-0064 and the surviving half of BUG-0009.
            //
            // Becoming current, not taking the role: who may act is the arbiter's to say, and
            // an instance that promoted itself on the strength of having caught up would be
            // deciding that for itself.
            reconciling_to_lead_ = false;
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
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: primary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == sequencer_er_secondary_conn_id_) {
        sequencer_er_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: secondary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == outbound_replication_conn_id_) {
        outbound_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-secondary replication connection {} lost: {} -- book updates paused until secondary reconnects", id.get_value(),
                   reason);
    } else if (id == inbound_replication_conn_id_) {
        inbound_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-primary replication connection {} lost: {} -- replica book is now stale (last seq={})", id.get_value(), reason,
                   last_replicated_seq_no_);
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
    if (pdu_id == pubsub_itc_fw_app::BookUpdate::message_pdu_id) {
        // BookUpdate from ME-primary -- secondary updates its replica book.
        apply_book_update(message);
        return; // apply_book_update calls release_pdu_payload internally.
    }

    if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported sequenced PDU id {} -- dropping", pdu_id);
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
    // A CancelBook or a TechnicalEvent command is gated as an order is: it changes the book or whether trading is halted,
    // so only the instance that leads applies it, and the follower learns of the change through the leader's book updates.
    const bool is_order_pdu = (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id));

    // ---- Order PDU gating by HA state -------------------------------------
    if (is_order_pdu && ha_role_state_ == MeRole::Follower) {
        // Passive follower: order PDUs from the sequencer are discarded (the primary
        // is authoritative; the follower's book is maintained via BookUpdate replication).
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: FOLLOWER -- discarding order PDU inner_pdu_id={} on connection {}",
                   inner_pdu_id, message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }
    if (is_order_pdu && ha_enabled_ && ha_role_state_ == MeRole::Unknown) {
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
        has_reconciled_ = false;
        release_pdu_payload(message);
        return;
    }

    if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        pubsub_itc_fw_app::NewOrderSingleView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode NewOrderSingle -- dropping");
            release_pdu_payload(message);
            return;
        }
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
        state_machine_.apply_new_order(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0,
                                       session_identity_from(envelope), source);

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        pubsub_itc_fw_app::OrderCancelRequestView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode OrderCancelRequest -- dropping");
            release_pdu_payload(message);
            return;
        }
        const CommandSource source = command_source_for(message.seq_no());
        state_machine_.apply_cancel(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0,
                                    session_identity_from(envelope), source);

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::CancelBook::message_pdu_id)) {
        pubsub_itc_fw_app::CancelBookView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode CancelBook -- dropping");
            release_pdu_payload(message);
            return;
        }
        const CommandSource source = command_source_for(message.seq_no());
        apply_cancel_book_command(view, message.seq_no(), envelope.wall_time_ns, envelope.has_trading_date ? envelope.trading_date : 0, source);

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)) {
        pubsub_itc_fw_app::TechnicalEventView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode TechnicalEvent -- dropping");
            release_pdu_payload(message);
            return;
        }
        const CommandSource source = command_source_for(message.seq_no());
        apply_technical_event_command(view, message.seq_no(), envelope.has_trading_date ? envelope.trading_date : 0, source);

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
        // The ME is the source of ERs, not a consumer -- discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: ExecutionReport envelope on connection {} -- discarding (ME does not consume ERs)",
                   message.connection_id().get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported envelope inner pdu_id {} -- dropping", inner_pdu_id);
    }

    release_pdu_payload(message);
}

CommandSource MatchingEngineThread::command_source_for(int64_t sequence_number) {
    if (ha_role_state_ != MeRole::Reconciling) {
        return CommandSource::live;
    }
    // Counted before the state machine decides anything, a refusal included: the catch-up is complete
    // when every record has been offered, and a record refused before it was counted would leave it
    // looking incomplete for ever (R-0101).
    ++reconciliation_records_seen_;
    catch_up_tally_.offer(sequence_number);
    return CommandSource::catch_up;
}

void MatchingEngineThread::EngineOutputs::send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number,
                                                      const fix_common::SessionIdentity& session, ReportIsRepeat repeat) {
    // Every report a catch-up produces is marked as a possible repeat, and no other report is, so
    // this counts the reports sent out of the catch-up.
    if (repeat == ReportIsRepeat::yes) {
        ++owner_.reconciliation_reports_sent_;
    }
    owner_.send_er_to_sequencer(report, sequence_number, session, repeat);
}

void MatchingEngineThread::EngineOutputs::order_added_to_book(int64_t sequence_number, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
                                                              const OrderEntry& entry) {
    owner_.send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Add, session, cl_ord_id, &entry);
}

void MatchingEngineThread::EngineOutputs::order_removed_from_book(int64_t sequence_number, const fix_common::SessionIdentity& session,
                                                                  std::string_view cl_ord_id) {
    owner_.send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Remove, session, cl_ord_id, nullptr);
}

void MatchingEngineThread::EngineOutputs::trading_halt_changed(int64_t sequence_number, bool halted) {
    owner_.send_book_update(sequence_number, halted ? pubsub_itc_fw_app::BookUpdateType::HaltSet : pubsub_itc_fw_app::BookUpdateType::HaltLifted,
                            fix_common::SessionIdentity{}, std::string_view{}, nullptr);
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
    return ha_enabled_ && ha_role_state_ == MeRole::Reconciling && !reconciling_to_lead_;
}

void MatchingEngineThread::release_held_reports() {
    if (held_reports_.empty()) {
        held_reports_overflowed_ = false;
        waiting_for_report_connection_ = false;
        return;
    }
    // With no connection to a sequencer the reports would go nowhere, so they stay held until one is
    // established, when this is called again.
    const bool to_primary = sequencer_er_conn_id_.is_valid();
    const bool to_secondary = sequencer_er_secondary_conn_id_.is_valid();
    if (!to_primary && !to_secondary) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: releasing {} report(s) held while this instance was becoming current -- it now serves, so the members who placed "
               "those orders are told",
               held_reports_.size());
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::ReportsReleased::message_pdu_id,
                                 pubsub_itc_fw_app::ReportsReleased{pubsub_itc_fw_app::ReleaseReason::NowServes, static_cast<int64_t>(held_reports_.size())});
    }
    send_held_reports(to_primary, to_secondary);
}

void MatchingEngineThread::send_held_reports(bool to_primary, bool to_secondary) {
    // Each report goes to each sequencer, as a report sent at once does, but a connection may come up
    // after the others: so each held report records which connections it has been sent on, is sent on
    // each as it becomes available, and is kept until it has been sent on every connection there is.
    // It keeps the number it was given when first sent, so that both sequencers see the same number
    // for it (docs/bug_list.md, BUG-0120).
    for (HeldReport& held : held_reports_) {
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = held.seq_no;
        envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
        envelope.payload.data = held.payload.data();
        envelope.payload.size = held.payload.size();
        envelope.has_sender_comp_id = !held.session.empty();
        envelope.sender_comp_id = held.session.comp_id_view();
        envelope.has_origin_gateway_id = !held.session.empty();
        envelope.origin_gateway_id = held.session.protocol;
        envelope.poss_resend = held.poss_resend;
        if (held.numbered) {
            envelope.has_report_engine_epoch = true;
            envelope.report_engine_epoch = held.report_engine_epoch;
            envelope.has_report_number = true;
            envelope.report_number = held.report_number;
        } else {
            number_report(envelope);
            held.numbered = true;
            held.report_engine_epoch = envelope.report_engine_epoch;
            held.report_number = envelope.report_number;
        }
        if (to_primary && !held.sent_to_primary) {
            send_pdu(sequencer_er_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, held.seq_no, envelope);
            held.sent_to_primary = true;
        }
        if (to_secondary && !held.sent_to_secondary) {
            send_pdu(sequencer_er_secondary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, held.seq_no, envelope);
            held.sent_to_secondary = true;
        }
    }
    // Without high availability there is only the one sequencer to tell.
    const bool secondary_expected = ha_enabled_;
    held_reports_.erase(
        std::remove_if(held_reports_.begin(), held_reports_.end(),
                       [secondary_expected](const HeldReport& held) { return held.sent_to_primary && (held.sent_to_secondary || !secondary_expected); }),
        held_reports_.end());
    if (held_reports_.empty()) {
        held_reports_overflowed_ = false;
        waiting_for_report_connection_ = false;
    }
}

void MatchingEngineThread::release_reports_held_for_a_connection() {
    // Reports held while this instance waits to be told whether it may serve stay held until it is
    // told. Any others go now: those held for want of a connection, and those an instance was told it
    // may serve while it had no connection to send them on.
    if (held_reports_.empty() || holding_reports_until_entitled()) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: a connection to a sequencer for reports is established -- sending {} report(s) held without one", held_reports_.size());
    if (test_events_.has_value()) {
        test_events_->send_event(
            pubsub_itc_fw_app::ReportsReleased::message_pdu_id,
            pubsub_itc_fw_app::ReportsReleased{pubsub_itc_fw_app::ReleaseReason::ConnectionMade, static_cast<int64_t>(held_reports_.size())});
    }
    send_held_reports(sequencer_er_conn_id_.is_valid(), sequencer_er_secondary_conn_id_.is_valid());
}

void MatchingEngineThread::number_report(pubsub_itc_fw_app::WalRecord& envelope) {
    // Where this report stands in the reports this engine sends, so that the leading sequencer can
    // say how far it has forwarded them and a follower can discard the copies it keeps
    // (docs/bug_list.md, BUG-0116). Numbered as it is sent, so the numbers follow the order the
    // sequencers receive the reports in. A new leadership numbers from 1 again under its higher epoch.
    if (report_number_epoch_ != epoch_) {
        report_number_epoch_ = epoch_;
        report_number_ = 0;
    }
    envelope.has_report_engine_epoch = true;
    envelope.report_engine_epoch = epoch_;
    envelope.has_report_number = true;
    envelope.report_number = ++report_number_;
}

void MatchingEngineThread::discard_held_reports(const char* reason) {
    if (held_reports_.empty()) {
        held_reports_overflowed_ = false;
        return;
    }
    // Said plainly rather than dropped quietly. These are reports a member would have received
    // had this instance gone the other way, and an operator reconciling a member's day against
    // this log should be able to see that they were not sent, and why.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: discarding {} held report(s) -- {}", held_reports_.size(), reason);
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::ReportsDiscarded::message_pdu_id,
                                 pubsub_itc_fw_app::ReportsDiscarded{static_cast<int64_t>(held_reports_.size())});
    }
    held_reports_.clear();
    held_reports_.shrink_to_fit();
    held_reports_overflowed_ = false;
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

    // Held rather than sent while this instance is becoming current without having been told it
    // may serve. See held_reports_ for why, and release_held_reports for what happens next.
    if (holding_reports_until_entitled()) {
        if (held_reports_.size() < max_held_reports_) {
            HeldReport held;
            held.payload.assign(er_encode_buffer_.begin(), er_encode_buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_written));
            held.seq_no = seq_no;
            held.session = session;
            held.poss_resend = repeat == ReportIsRepeat::yes;
            held_reports_.push_back(std::move(held));
            return;
        }
        if (!held_reports_overflowed_) {
            held_reports_overflowed_ = true;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: {} reports held while waiting to be told what this instance may do, which is as many as are kept -- the rest "
                       "are sent as they are produced. A member may receive a report for an order this instance never serves; it is marked as a possible "
                       "repeat and can be discarded, which is the lesser harm",
                       held_reports_.size());
        }
    }

    // Held, too, while there is no connection to a sequencer to send it on: the order it reports is in
    // the sequencer's log and on this book, so the member must be told once a connection exists.
    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        if (!waiting_for_report_connection_) {
            waiting_for_report_connection_ = true;
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "MatchingEngineThread: no connection to a sequencer for reports -- acting on orders and holding their reports until one is "
                           "established");
            if (test_events_.has_value()) {
                test_events_->send_event(pubsub_itc_fw_app::ReportsHeld::message_pdu_id,
                                         pubsub_itc_fw_app::ReportsHeld{static_cast<int64_t>(held_reports_.size()) + 1});
            }
        }
        if (held_reports_.size() < max_held_reports_) {
            HeldReport held;
            held.payload.assign(er_encode_buffer_.begin(), er_encode_buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_written));
            held.seq_no = seq_no;
            held.session = session;
            held.poss_resend = repeat == ReportIsRepeat::yes;
            held_reports_.push_back(std::move(held));
        } else if (!held_reports_overflowed_) {
            held_reports_overflowed_ = true;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: {} reports held with no connection to a sequencer, which is as many as are kept -- later reports are "
                       "lost until a connection is established",
                       held_reports_.size());
        }
        return;
    }

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = seq_no;
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    envelope.payload.data = er_encode_buffer_.data();
    envelope.payload.size = bytes_written;
    envelope.has_sender_comp_id = !session.empty();
    envelope.sender_comp_id = session.comp_id_view();
    envelope.has_origin_gateway_id = !session.empty();
    envelope.origin_gateway_id = session.protocol;
    envelope.poss_resend = repeat == ReportIsRepeat::yes;

    // The time the gateway read the order that caused this report, sent back with it so the
    // sequencer can time its own handling of the return leg. Absent for every report with no
    // client order behind it, which is what has_current_order_ingress_ns_ records.
    //
    // This is not how the gateway learns the time. The sequencer keeps its own copy, filed
    // under the order's sequence number, and writes that onto the report it forwards --
    // overwriting whatever arrives here. That copy stays the authoritative one because it
    // survives cases this one cannot, such as a report for an order placed before the
    // matching engine now running had started.
    envelope.has_gateway_ingress_ns = has_current_order_ingress_ns_;
    envelope.gateway_ingress_ns = current_order_ingress_ns_;

    number_report(envelope);

    if (sequencer_er_conn_id_.is_valid()) {
        // A member is waiting for this: it is the report on its way back to them.
        send_pdu(sequencer_er_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq_no, envelope,
                 pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
    }
    if (sequencer_er_secondary_conn_id_.is_valid()) {
        send_pdu(sequencer_er_secondary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq_no, envelope,
                 pubsub_itc_fw::MemberIsWaitingFlag{pubsub_itc_fw::MemberIsWaitingFlag::MemberIsWaiting});
    }

    // The order has now been matched and its acknowledgement handed to the reactor, so this
    // is the moment the matching engine is finished with it.
    //
    // Only an acknowledgement of a new order is timed, and that restriction is what makes
    // reading the remembered time safe rather than merely likely to be right. This function
    // is reached for every report the state machine produces -- reports that a halt has
    // cancelled an order, reports from a catch-up, refusals -- and only an acknowledgement of
    // a new order has a client order behind it whose start time could be meant. Every other
    // report carries a status other than New, so none of them reaches the line below,
    // whether or not an order happens to be being handled at the time.
    if (er.ord_status == pubsub_itc_fw_app::OrdStatus::New) {
        order_path_metrics::observe_checkpoint(er_out_elapsed_histogram_, has_current_order_ingress_ns_, current_order_ingress_ns_,
                                               config_.wall_clock->now_ns());
    }
}

void MatchingEngineThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == book_metrics_timer_id_) {
        publish_book_metrics();
        return;
    }

    if (id == reconciliation_timer_id_) {
        if (ha_role_state_ != MeRole::Reconciling) {
            cancel_timer(reconciliation_timer_id_);
            return;
        }
        ++reconciliation_attempts_;
        if (sequencer_order_conn_ids_.empty()) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: RECONCILING for {} attempt(s) with no sequencer order connection -- the venue has no matching engine until one "
                       "connects and answers",
                       reconciliation_attempts_);
            return;
        }
        if (reconciliation_records_seen_ != reconciliation_records_at_last_tick_) {
            // Records are arriving, so a sequencer is serving this catch-up and the silence is
            // its length rather than its absence. Asking again here would have the whole
            // catch-up sent a second time, arriving after promotion and processed as live.
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: RECONCILING -- {} record(s) applied so far, catch-up still arriving", reconciliation_records_seen_);
            reconciliation_records_at_last_tick_ = reconciliation_records_seen_;
            return;
        }
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: not caught up and no record after {} attempt(s) -- asking again. A sequencer that does not lead declines and "
                   "says when it leads; this is for when that message, or the answer, did not come",
                   reconciliation_attempts_);
        send_follow_request();
        return;
    }

    if (id == halt_request_timer_id_) {
        // A request lost to a change of sequencer leader is asked for again.
        if (halt_awaited_ && !state_machine_.halted()) {
            request_halt();
        }
        return;
    }

    if (id == lease_tick_timer_id_) {
        act_on(lease_agent_->on_tick(std::chrono::steady_clock::now()));
        // A request for a cancel of the book that a change of sequencer leader lost is asked for again.
        if (cancel_book_awaited_through_ != 0 && ha_role_state_ == MeRole::Leader &&
            std::chrono::steady_clock::now() - cancel_book_requested_at_ >= cancel_book_request_interval) {
            request_cancel_book();
        }
        return;
    }
}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void MatchingEngineThread::send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, const fix_common::SessionIdentity& session,
                                            std::string_view cl_ord_id, const OrderEntry* entry) {
    if (!ha_enabled_ || !is_primary_ || !outbound_replication_conn_id_.is_valid()) {
        return;
    }

    pubsub_itc_fw_app::BookUpdate upd{};
    upd.seq_no = seq_no;
    upd.update_type = static_cast<int8_t>(update_type);
    // The identity, on both Add and Remove: the replica keys its book by it, so a Remove
    // that named only a ClOrdID could not find the entry to erase.
    upd.comp_id = session.comp_id_view();
    upd.origin_gateway_id = session.protocol;
    upd.cl_ord_id = cl_ord_id;

    if (entry != nullptr) {
        upd.placed_at_seq_no = entry->placed_at_seq_no;
        upd.side = static_cast<int8_t>(entry->side);
        upd.ord_type = static_cast<int8_t>(entry->ord_type);
        upd.symbol = entry->get_symbol();
        upd.order_qty = entry->get_order_qty();
        if (entry->has_time_in_force) {
            upd.has_time_in_force = true;
            upd.time_in_force = static_cast<int8_t>(entry->time_in_force);
        }
        if (entry->has_expire_time) {
            upd.has_expire_time = true;
            upd.expire_time = entry->expire_time;
        }
        if (entry->has_price) {
            upd.has_price = true;
            upd.price = entry->get_price();
        }
    }

    send_pdu(outbound_replication_conn_id_, pubsub_itc_fw_app::BookUpdate::message_pdu_id, seq_no, upd);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: BookUpdate sent seq={} type={} comp_id='{}' cl_ord_id={}", seq_no,
               static_cast<int>(update_type), session.comp_id_view(), cl_ord_id);
}

void MatchingEngineThread::apply_book_update(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::BookUpdateView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode BookUpdate -- dropping");
        release_pdu_payload(message);
        return;
    }

    const fix_common::SessionIdentity session = fix_common::SessionIdentity::make(view.comp_id, view.origin_gateway_id);
    const OrderKey key = OrderKey::make(session, view.cl_ord_id);
    const auto update_type = static_cast<pubsub_itc_fw_app::BookUpdateType>(view.update_type);

    if (update_type == pubsub_itc_fw_app::BookUpdateType::Add) {
        OrderEntry entry{};
        entry.placed_at_seq_no = view.placed_at_seq_no;
        entry.session = session;
        entry.has_time_in_force = view.has_time_in_force;
        entry.time_in_force = static_cast<pubsub_itc_fw_app::TimeInForce>(view.time_in_force);
        entry.has_expire_time = view.has_expire_time;
        entry.expire_time = view.expire_time;
        entry.side = static_cast<pubsub_itc_fw_app::Side>(view.side);
        entry.ord_type = static_cast<pubsub_itc_fw_app::OrdType>(view.ord_type);
        entry.set_symbol(view.symbol);
        entry.set_order_qty(view.order_qty);
        entry.has_price = view.has_price;
        if (view.has_price) {
            entry.set_price(view.price);
        }
        if (!order_book_.add_or_replace(key, entry, view.seq_no)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: replica cannot apply Add seq={} cl_ord_id={} -- the order book region is full at {} order(s)", view.seq_no,
                       view.cl_ord_id, order_book_.region_capacity());
            release_pdu_payload(message);
            return;
        }
        // The follower sends nothing outward, so there is no report to get ahead of.
        order_book_.publish(view.seq_no);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: replica Add seq={} cl_ord_id={} book_size={}", view.seq_no,
                   view.cl_ord_id, order_book_.size());
    } else if (update_type == pubsub_itc_fw_app::BookUpdateType::Remove) {
        order_book_.remove(key);
        order_book_.publish(view.seq_no);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: replica Remove seq={} cl_ord_id={} book_size={}", view.seq_no,
                   view.cl_ord_id, order_book_.size());
    } else if (update_type == pubsub_itc_fw_app::BookUpdateType::HaltSet || update_type == pubsub_itc_fw_app::BookUpdateType::HaltLifted) {
        // The leader applied a command that halted trading or lifted a halt. Recorded before the position is published,
        // as the leader does, so that this instance refuses orders as the leader does if it is promoted.
        const bool halted = update_type == pubsub_itc_fw_app::BookUpdateType::HaltSet;
        order_book_.set_halted(halted ? TradingHalted::yes : TradingHalted::no);
        order_book_.publish(view.seq_no);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: replica seq={} -- the leader says trading is {}", view.seq_no,
                   halted ? "halted" : "no longer halted");
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unknown BookUpdate type {} -- dropping", view.update_type);
    }

    const int64_t prev_seq = last_replicated_seq_no_;
    last_replicated_seq_no_ = view.seq_no;

    // Log the first update and every 10,000 seq_nos so progress is visible
    // without flooding the log at Info level.
    if (prev_seq == 0 || view.seq_no % 10000 == 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: replica book seq={} size={}", view.seq_no, order_book_.size());
    }

    release_pdu_payload(message);
}

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
    if (ha_role_state_ == MeRole::Leader) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: adopting LEADER role (epoch={})", epoch_);
    report_role_change(ha_role_state_, MeRole::Leader);
    ha_role_state_ = MeRole::Leader;
    announce_role();

    // This instance is going to serve, so anything it caught up on and held back is owed to the
    // members who placed those orders.
    release_held_reports();
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
            reconciling_to_lead_ = false;
            // The command it asked for, if any, is the next leader's to ask for; this instance no longer waits for it.
            cancel_book_awaited_through_ = 0;
            if (halt_awaited_) {
                halt_awaited_ = false;
                cancel_timer(halt_request_timer_id_);
                halt_request_timer_id_ = pubsub_itc_fw::TimerID{};
            }
            enter_follower_state();
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::AgreedPeerLeads:
            if (ha_role_state_ == MeRole::Reconciling && !reconciling_to_lead_) {
                // Catching up at startup: follow once the catch-up ends.
                peer_leader_heard_while_reconciling_ = true;
                peer_leader_instance_id_ = peer_instance_id();
                peer_leader_epoch_ = lease_agent_->highest_epoch();
            } else if (ha_role_state_ == MeRole::Unknown) {
                static_cast<void>(follow_peer_claiming_leadership(peer_instance_id(), lease_agent_->highest_epoch()));
            }
            break;
        case pubsub_itc_fw::PairLeaseAgent::Change::Nothing:
            break;
    }
}

void MatchingEngineThread::EngineLeaseLinks::send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) {
    const pubsub_itc_fw::ConnectionID peer =
        owner_.outbound_replication_conn_id_.is_valid() ? owner_.outbound_replication_conn_id_ : owner_.inbound_replication_conn_id_;
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
    event.records_applied = reconciliation_records_seen_;
    event.records_sent = records_sent;
    event.reports_sent = reconciliation_reports_sent_;
    event.book_size = static_cast<int64_t>(order_book_.size());
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
    if (ha_role_state_ == MeRole::Reconciling) {
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::Started, last_replicated_seq_no_, 0);
        return;
    }
    if (!has_reconciled_) {
        return;
    }
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Completed, last_replicated_seq_no_, last_catch_up_records_sent_);
    if (ha_role_state_ == MeRole::Unknown) {
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::CurrentAndWaiting, last_replicated_seq_no_, last_catch_up_records_sent_);
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
    const MeRole previous = ha_role_state_;
    ha_role_state_ = MeRole::Follower;
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
    discard_held_reports("this instance is a follower and serves nobody");

    announce_role();
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
    if (!peer_leader_heard_while_reconciling_) {
        return;
    }
    // Consumed whether or not it is followed, so a claim heard once cannot be applied twice and
    // cannot outlive the catch-up it arrived during.
    peer_leader_heard_while_reconciling_ = false;
    const int64_t instance_id = peer_leader_instance_id_;
    const int32_t announced_epoch = peer_leader_epoch_;
    // Re-checked against the epoch as it stands now, not as it stood when the announcement
    // arrived: reconciliation can have advanced this instance's own generation in between.
    follow_peer_claiming_leadership(instance_id, announced_epoch);
}

void MatchingEngineThread::announce_role() {
    // Reconciling counts as follower to the outside world: this instance has been told it
    // will lead but cannot serve until its book is caught up, and orders sent meanwhile
    // would be dropped. It announces LEADER from adopt_leader_role, once it can.
    if (ha_role_state_ != MeRole::Leader && ha_role_state_ != MeRole::Follower) {
        return;
    }
    pubsub_itc_fw_app::RoleAnnouncement announcement{};
    announcement.instance_id = static_cast<int64_t>(config_.instance_id);
    announcement.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    announcement.current_role = ha_role_state_ == MeRole::Leader ? pubsub_itc_fw_app::Role::leader : pubsub_itc_fw_app::Role::follower;
    announcement.epoch = epoch_;

    // The peer gets this too, not just the sequencers. An instance starting beside
    // an established leader can then adopt follower on the strength of the leader
    // saying so, instead of waiting on arbitration that a recently started arbiter
    // will decline. Both replication connections are held permanently in both
    // directions, so whichever one is up carries it.
    for (const pubsub_itc_fw::ConnectionID& conn :
         {sequencer_er_conn_id_, sequencer_er_secondary_conn_id_, outbound_replication_conn_id_, inbound_replication_conn_id_}) {
        if (conn.is_valid()) {
            send_pdu(conn, pubsub_itc_fw_app::RoleAnnouncement::message_pdu_id, 0, announcement);
        }
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: announced role {} at epoch {} to the sequencers and the peer",
               pubsub_itc_fw_app::to_string(announcement.current_role), epoch_);
}

// WAL reconciliation (Slice D)

void MatchingEngineThread::recover_open_orders(bool region_existed) {
    if (region_was_unusable_) {
        // The region this process inherited could not be used, so this one is new, but the engine is not: it held orders,
        // and the venue must say what became of them. It asks for every record from the first, so that the catch-up
        // rebuilds the book from the WAL (R-0123); account_for_unusable_region decides what follows.
        order_book_.warm();
        has_position_ = true;
        last_replicated_seq_no_ = 0;
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
        has_position_ = false;
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: no order book region was left behind -- starting with an empty book and no position of its own");
        return;
    }
    has_position_ = true;

    const auto start = std::chrono::steady_clock::now();
    const OrderBook::Recovery found = order_book_.recover();
    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // The scan above read every page, so there is nothing left for warm() to do.

    // Where the tail starts. The region is current to what it published and no further, so
    // everything the sequencer holds after that has still to be applied -- which is the
    // existing reconciliation path, asked to start here instead of at zero.
    last_replicated_seq_no_ = found.published;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: recovered {} open order(s) from the region at seq_no={} in {} ms ({} unfinished record(s) discarded)", found.orders,
               found.published, elapsed_ms, found.discarded);

    if (order_book_.halted()) {
        // Set by a command the engine applied before it stopped, and kept in the region with the orders.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: the region records that trading is halted -- the venue refuses every new order until a person lifts the halt "
                       "(R-0023)");
    }
}

void MatchingEngineThread::become_leader_when_current() {
    if (ha_role_state_ == MeRole::Leader) {
        // Already serving: it has applied every order as it arrived, and catching up now would
        // make the live orders arriving meanwhile read as replay.
        return;
    }
    // Every other instance catches up first, whatever it has done before. One that caught up when
    // the first sequencer connected, and has waited since to learn its role, was current only at
    // that moment: a sequencer sends orders only to the engine that leads, so every order accepted
    // while it waited reached it only through the WAL. Leading without catching up would lose
    // each of them, and tell no member (BUG-0198).
    reconciling_to_lead_ = true;
    begin_reconciliation();
}

void MatchingEngineThread::begin_reconciliation() {
    if (ha_role_state_ != MeRole::Reconciling) {
        // Read before the state is overwritten, and only on the way in: this is re-entered
        // when the sequencer's connection arrives, and a re-entry must not turn a promotion
        // into a start.
        reconciling_from_follower_ = ha_role_state_ == MeRole::Follower;
    }
    ha_role_state_ = MeRole::Reconciling;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: entering RECONCILING (last_replicated_seq_no={}, book_size={})",
               last_replicated_seq_no_, order_book_.size());
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Started, last_replicated_seq_no_, 0);

    // The sequencers connect to our (now-listening) order port. If at least one is
    // already connected, request WAL catch-up immediately; otherwise
    // on_connection_established will call begin_reconciliation() again once a
    // connection is up.
    follow_request_sent_ = false;
    if (!sequencer_order_conn_ids_.empty()) {
        send_follow_request();
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: RECONCILING -- awaiting sequencer order connection before WAL catch-up");
    }

    // Only a sequencer that leads serves a catch-up; one still deciding drops the request and
    // says nothing. An instance starting with the whole venue asks at exactly that moment, so
    // the request is repeated until it is answered. It is never abandoned: acting without the
    // catch-up is the loss this exists to prevent, and there is no answer to fall back on. A
    // venue whose sequencers are not leading is not trading either, so waiting costs nothing
    // that is not already lost.
    reconciliation_attempts_ = 0;
    reconciliation_records_seen_ = 0;
    reconciliation_records_at_last_tick_ = 0;
    reconciliation_reports_sent_ = 0;
    cancel_timer(reconciliation_timer_id_);
    reconciliation_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.catch_up_retry_seconds));
}

pubsub_itc_fw::WalFollowRequest MatchingEngineThread::follow_request(const std::string& follower_name) const {
    pubsub_itc_fw::WalFollowRequest request{};
    request.follower_name = follower_name;
    // A negative position says "I hold nothing and have applied nothing of yours": place me at
    // your head rather than sending me your history. The alternative reading of an instance
    // with no region -- that it is a component at position zero which has fallen the whole day
    // behind -- would have the venue replay everything it still holds into an engine that never
    // held any of it.
    request.last_seq_no = has_position_ ? last_replicated_seq_no_ : -1;
    // Why this instance is asking. A promotion needs the sequencer to route orders here
    // afterwards, and this request is the only thing that tells it to -- the arbiter's decision
    // does not reach the sequencer. A start needs the records and nothing else: this instance may
    // be about to learn from its peer that it is a follower, and a sequencer that moved the order
    // connection here on the strength of the ask would leave the venue routing orders to an
    // instance that will not act on them. See BUG-0077 and ha_test.py scenario 54.
    request.wants_new_records = reconciling_to_lead_;
    return request;
}

void MatchingEngineThread::send_follow_request() {
    if (sequencer_order_conn_ids_.empty()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: cannot ask to be caught up -- no sequencer order connection");
        return;
    }
    // Sent to every sequencer order connection. Only the leading sequencer serves it; one that does not
    // lead declines, re-points its own order connection, and says when it leads (WalLeaderChanged), so
    // that this instance receives exactly one catch-up whichever connection the leader owns.
    // The message refers to the name rather than copying it, so the name must outlive the sends below.
    const std::string follower_name = "matching_engine_" + std::to_string(config_.instance_id);
    const pubsub_itc_fw::WalFollowRequest request = follow_request(follower_name);
    // A repeated request is answered with the whole stream again, so the tally counts from this
    // ask rather than from the start of the reconciliation. R-0101.
    catch_up_tally_ = pubsub_itc_fw::CatchUpTally(request.last_seq_no);
    for (const auto& conn_id : sequencer_order_conn_ids_) {
        send_pdu(conn_id, pubsub_itc_fw::WalFollowRequest::message_pdu_id, 0, request);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: asked on connection {} to be caught up (last_seq_no={}, {})",
                   conn_id.get_value(), last_replicated_seq_no_, reconciling_to_lead_ ? "asking to lead" : "starting, not asking to lead");
    }
    follow_request_sent_ = true;
}

void MatchingEngineThread::ask_on_new_sequencer_connection(const pubsub_itc_fw::ConnectionID& id) {
    if (!follow_request_sent_) {
        begin_reconciliation();
        return;
    }
    // The catch-up has been asked for on the other connections already. The leading sequencer serves every request it
    // is sent, so asking on those again would have it start its stream again from the first record, while the first
    // stream is still arriving, and this instance would find the records going backwards and ask again only after
    // catch_up_retry_seconds (BUG-0200). So only this connection is asked, and the tally is kept: if this connection's
    // sequencer leads, the others declined and nothing has arrived to count; if it does not, it declines, and the
    // stream under way finishes as it would have. A leader whose own connection was lost and made again in the middle
    // of a stream sends the rest on this one, and the tally then counts more records than it says it sent; the
    // catch-up is judged incomplete and asked for again, as before.
    const std::string follower_name = "matching_engine_" + std::to_string(config_.instance_id);
    const pubsub_itc_fw::WalFollowRequest request = follow_request(follower_name);
    send_pdu(id, pubsub_itc_fw::WalFollowRequest::message_pdu_id, 0, request);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: asked on new connection {} to be caught up (last_seq_no={}, {}), leaving the catch-up asked for on the others to finish",
               id.get_value(), last_replicated_seq_no_, reconciling_to_lead_ ? "asking to lead" : "starting, not asking to lead");
}

void MatchingEngineThread::handle_follow_declined(const pubsub_itc_fw::EventMessage& message) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: the sequencer on connection {} does not lead and declined to catch this instance up -- it says when it leads",
               message.connection_id().get_value());
}

void MatchingEngineThread::handle_leader_changed(const pubsub_itc_fw::EventMessage& message) {
    if (ha_role_state_ != MeRole::Reconciling) {
        return; // caught up already: nothing to ask for
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: the sequencer on connection {} now leads -- asking again to be caught up",
               message.connection_id().get_value());
    send_follow_request();
}

void MatchingEngineThread::send_engine_settings() {
    if (!outbound_replication_conn_id_.is_valid()) {
        return;
    }
    pubsub_itc_fw_app::EngineSettings settings{};
    settings.instance_id = static_cast<int64_t>(config_.instance_id);
    settings.order_book_region_capacity = static_cast<int64_t>(order_book_.region_capacity());
    settings.identifier_secret_fingerprint = report_identifiers_.secret_fingerprint();
    send_pdu(outbound_replication_conn_id_, pubsub_itc_fw_app::EngineSettings::message_pdu_id, 0, settings);
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
    const bool acting_on_orders = !ha_enabled_ || ha_role_state_ == MeRole::Leader;
    if (!acting_on_orders) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: EnginePositionQuery {} on connection {} -- not acting on orders (state={}), not answering", query.request_id,
                   message.connection_id().get_value(), static_cast<int>(ha_role_state_));
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
    if (ha_role_state_ != MeRole::Reconciling) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: already promoted (state={}) -- ignoring a duplicate caught-up message",
                   static_cast<int>(ha_role_state_));
        return;
    }

    // R-0101: establish that the catch-up was complete before acting on it. What is checked is
    // that every record the sequencer SENT arrived, exactly once and in ascending order -- not
    // that the numbers run consecutively, because the stream is filtered and the gaps between
    // the numbers that arrive are ordinary. See CatchUpTally, which says what that does and does
    // not establish.
    //
    // Nothing is promoted, reconciled or cancelled on a catch-up that cannot be accounted for.
    // The reconciliation timer is deliberately left running: the next tick finds that nothing
    // has arrived since the last ask and repeats the request, so an incomplete stream is retried
    // rather than becoming a state the engine sits in. Applying the same records twice is
    // absorbed -- an order already on the book is not added again.
    if (!catch_up_tally_.complete_with(ack.records_sent, ack.last_seq_no)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: catch-up was not complete -- {}. Not acting on it, and asking again (R-0101). {} record(s) were applied and are "
                   "kept, because what arrived was not wrong, only incomplete",
                   catch_up_tally_.describe_shortfall(ack.records_sent, ack.last_seq_no), reconciliation_records_seen_);
        report_catch_up(pubsub_itc_fw_app::CatchUpStage::Incomplete, ack.last_seq_no, ack.records_sent);
        return;
    }

    cancel_timer(reconciliation_timer_id_);
    last_replicated_seq_no_ = ack.last_seq_no;
    state_machine_.note_caught_up_to(ack.last_seq_no);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: caught up -- book reconciled to seq_no={} (book_size={}), {} record(s) applied and {} report(s) sent to "
               "members, each marked as a possible repeat; the catch-up accounted for all {} record(s) sent (R-0101)",
               ack.last_seq_no, order_book_.size(), reconciliation_records_seen_, reconciliation_reports_sent_, ack.records_sent);
    last_catch_up_records_sent_ = ack.records_sent;
    report_catch_up(pubsub_itc_fw_app::CatchUpStage::Completed, ack.last_seq_no, ack.records_sent);

    // The book is now assembled: what the region vouched for, plus every record the sequencer
    // held after it. What happens to it next is a stated policy rather than a decision taken
    // here -- order_book.open_orders_on_promotion, "cancel" or "keep".
    //
    // "keep" is R-0018 and R-0073: an order still on the book is genuinely outstanding, so the
    // member goes on holding what it placed. It is the better outcome and it rests on the book
    // being the one the venue had, which nothing yet establishes -- neither the replica this
    // instance maintained nor the completeness of the catch-up that followed it (R-0101).
    //
    // "cancel" tells every member its orders are gone, which is worse and is certain. It is the
    // deployed default until the checks behind "keep" exist. See R-0020, R-0073, and
    // docs/availability/wal_and_ha.md.
    //
    // Where the region could not be used, R-0123 decides first: the book the catch-up rebuilt is kept if the WAL
    // reached the first command of the day, and otherwise trading halts. A rebuilt book is the venue's, so what
    // follows applies to it as to any other.
    if (!book_can_be_vouched_for()) {
        account_for_unusable_region(ack);
    }
    if (!book_can_be_vouched_for()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: holding {} order(s) it cannot vouch for -- not keeping or cancelling them as a promotion would", order_book_.size());
    } else if (reconciling_from_follower_ && cancel_open_orders_on_promotion_) {
        // The book could have been carried across. It is not, because the configuration says the
        // venue does not promise that yet: each member is told its order is gone rather than
        // being left to trust a book nothing has checked.
        //
        // The cancel is a command in the WAL, written by the leading sequencer at this instance's request, so that every
        // engine applies it at the same position, one that catches up later included (BUG-0199). It names the position
        // this instance caught up to, so that only the orders it inherited are cancelled: an order that reaches the
        // sequencer after the catch-up stays on the book, as it would have before (decision 7 of
        // docs/availability/deterministic_matching_engine.md).
        if (order_book_.size() == 0) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                           "MatchingEngineThread: open_orders_on_promotion=cancel -- no order was inherited at this promotion, so there is nothing to cancel");
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: open_orders_on_promotion=cancel -- asking the leading sequencer for a command to cancel the {} order(s) "
                       "inherited at this promotion, placed at or before seq={}, rather than keeping them (R-0020; R-0073 is not being met, by "
                       "configuration)",
                       order_book_.size(), ack.last_seq_no);
            cancel_book_awaited_through_ = ack.last_seq_no;
            request_cancel_book();
        }
    } else if (reconciling_from_follower_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: open_orders_on_promotion=keep -- carrying {} open order(s) across the promotion (R-0073)", order_book_.size());
    } else {
        // A start rather than a promotion. This instance took no book over from a peer: what it
        // holds it read from its own region and caught up on top of, which is R-0018 and is not
        // what the promotion policy speaks about.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: started rather than promoted -- keeping the {} open order(s) recovered from the region (R-0018)", order_book_.size());
    }

    has_reconciled_ = true;
    has_position_ = true;
    ha_role_state_ = MeRole::Unknown; // clear reconciling

    if (!reconciling_to_lead_) {
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
               "MatchingEngineThread: reconciliation complete at seq_no={} with {} order(s) on the book -- resuming as leader{}", ack.last_seq_no,
               order_book_.size(), cancel_book_awaited_through_ != 0 ? ", and applying orders while the command to cancel what it inherited arrives" : "");
    resumed_through_ = ack.last_seq_no;
    // Said to the test once the book is the one this instance resumes with: at once, or, where it asked for its inherited
    // orders to be cancelled, once it has applied that command.
    if (cancel_book_awaited_through_ == 0) {
        report_resumed_as_leader();
    }
    adopt_leader_role();
}

bool MatchingEngineThread::book_can_be_vouched_for() const {
    // The region is what vouches for the book: it holds the orders themselves, and what it
    // published says how far they can be trusted. Where the one this process inherited could
    // not be read, the engine holds only what the sequencer's record supplied, which is a
    // different thing and must not be traded on as though it were the same.
    return order_book_.is_open() && !region_was_unusable_;
}

void MatchingEngineThread::account_for_unusable_region(const pubsub_itc_fw::WalFollowCaughtUpView& ack) {
    // The catch-up was asked for from the first record, so the book holds every order the WAL holds. Whether that is
    // every order the venue holds is whether the WAL begins with the first command of the trading day.
    if (ack.first_seq_no_held <= 1) {
        region_was_unusable_ = false;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the order book region could not be used, and the book was rebuilt from the WAL, which reaches the first command of "
                   "the trading day -- carrying on with the {} order(s) it holds through seq_no={}, cancelling nothing, because it is exactly the venue's book "
                   "(R-0123)",
                   order_book_.size(), ack.last_seq_no);
        if (test_events_.has_value()) {
            test_events_->send_event(pubsub_itc_fw_app::RegionRebuilt::message_pdu_id,
                                     pubsub_itc_fw_app::RegionRebuilt{ack.last_seq_no, static_cast<int64_t>(order_book_.size())});
        }
        return;
    }
    // Orders taken before the WAL's first record cannot be named, so this book is not the venue's. Cancelling the orders
    // it can name would leave every other one unmentioned, which is the silence R-0020 exists to prevent, reached while
    // appearing to have dealt with it; so nothing is cancelled.
    if (ha_enabled_ && !reconciling_to_lead_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the order book region could not be used, and the WAL starts at seq_no={}, so the orders taken before it cannot be "
                   "named -- this instance follows, so the engine that leads goes on trading; if this instance is to lead, it halts trading first (R-0123)",
                   ack.first_seq_no_held);
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
               "MatchingEngineThread: cannot account for what the venue is holding -- the order book region could not be used, and the WAL starts at "
               "seq_no={}, so the orders taken before it cannot be named. Asking the leading sequencer to halt trading, cancelling nothing (R-0123)",
               ack.first_seq_no_held);
    halt_awaited_ = true;
    request_halt();
    if (!halt_request_timer_id_.is_valid()) {
        halt_request_timer_id_ = start_recurring_timer(halt_request_interval);
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

void MatchingEngineThread::request_cancel_book() {
    pubsub_itc_fw_app::CancelBookRequest request{};
    request.instance_id = static_cast<int64_t>(config_.instance_id);
    request.epoch = epoch_;
    request.orders_placed_through = cancel_book_awaited_through_;
    // To every sequencer order connection, as a catch-up request is: only the leading sequencer writes the command.
    for (const auto& conn_id : sequencer_order_conn_ids_) {
        send_pdu(conn_id, pubsub_itc_fw_app::CancelBookRequest::message_pdu_id, 0, request);
    }
    cancel_book_requested_at_ = std::chrono::steady_clock::now();
}

void MatchingEngineThread::apply_cancel_book_command(const pubsub_itc_fw_app::CancelBookView& command, int64_t sequence_number, int64_t transact_time,
                                                     int32_t trading_date, CommandSource source) {
    const int64_t orders_placed_through = command.orders_placed_through;
    const HaltIfOrdersCancelled halt_if_orders_cancelled = command.halt_if_orders_cancelled ? HaltIfOrdersCancelled::yes : HaltIfOrdersCancelled::no;
    const MatchingEngineStateMachine::BookCancelledByCommand result =
        state_machine_.apply_cancel_book(orders_placed_through, halt_if_orders_cancelled, sequence_number, transact_time, trading_date, source);
    if (!result.applied) {
        return;
    }
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::BookCancelled::message_pdu_id,
                                 pubsub_itc_fw_app::BookCancelled{result.open_orders, result.reports_sent, true, result.orders_kept});
    }
    if (command.reason == pubsub_itc_fw_app::CancelBookReason::UnableToMatchTooLong) {
        // The orders are not stale because they are old. They are stale because the members that placed them were locked
        // out of them while the market moved: no engine could trade, so they could not cancel.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEngineThread: the leading sequencer wrote at seq={} that no matching engine could trade for longer than the venue allows -- {} "
                   "order(s) cancelled and their members told{} (R-0117)",
                   sequence_number, result.reports_sent,
                   result.halted             ? ", and trading is halted until a person lifts it (R-0023)"
                   : state_machine_.halted() ? "; trading was halted already"
                                             : "; trading carries on, because none were open");
    }
    if (result.halted) {
        halt_reason_ = pubsub_itc_fw_app::HaltReason::UnableToMatchTooLong;
        if (test_events_.has_value()) {
            test_events_->send_event(pubsub_itc_fw_app::Halted::message_pdu_id, pubsub_itc_fw_app::Halted{*halt_reason_});
        }
    }
    if (cancel_book_awaited_through_ != 0 && orders_placed_through >= cancel_book_awaited_through_) {
        cancel_book_awaited_through_ = 0;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: the {} order(s) inherited at this promotion are cancelled by the command at seq={} -- resumed as leader with "
                   "{} order(s) on the book",
                   result.reports_sent, sequence_number, order_book_.size());
        if (source == CommandSource::live) {
            report_resumed_as_leader();
        }
    }
}

void MatchingEngineThread::apply_technical_event_command(const pubsub_itc_fw_app::TechnicalEventView& event, int64_t sequence_number, int32_t trading_date,
                                                         CommandSource source) {
    const MatchingEngineStateMachine::TechnicalEventApplied result = state_machine_.apply_technical_event(event, sequence_number, trading_date, source);
    if (!result.applied || !result.halt_changed) {
        return;
    }
    if (event.halted) {
        if (halt_awaited_) {
            halt_awaited_ = false;
            cancel_timer(halt_request_timer_id_);
            halt_request_timer_id_ = pubsub_itc_fw::TimerID{};
        }
        if (event.reason == pubsub_itc_fw_app::TechnicalEventReason::CannotAccountForBook) {
            halt_reason_ = pubsub_itc_fw_app::HaltReason::CannotAccountForBook;
            if (test_events_.has_value()) {
                test_events_->send_event(pubsub_itc_fw_app::Halted::message_pdu_id, pubsub_itc_fw_app::Halted{*halt_reason_});
            }
        }
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "MatchingEngineThread: trading is halted -- the venue is not accepting orders and a person must lift this (R-0023)");
    } else {
        halt_reason_.reset();
    }
}

void MatchingEngineThread::report_resumed_as_leader() {
    if (test_events_.has_value()) {
        test_events_->send_event(pubsub_itc_fw_app::ResumedAsLeader::message_pdu_id,
                                 pubsub_itc_fw_app::ResumedAsLeader{resumed_through_, static_cast<int64_t>(order_book_.size())});
    }
}

} // namespaces
