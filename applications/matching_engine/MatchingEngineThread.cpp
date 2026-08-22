// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

#include <chrono>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace matching_engine {

namespace {

// PDU id for BookUpdate messages (must match the id in matching_engine_replication.dsl).

// Leader-follower protocol PDU ids (must match leader_follower.dsl).

// Echo the NoUnderlyings repeating group from a decoded NewOrderSingle onto its
// ExecutionReport. The decoded view carries UnderlyingsView elements; the ER expects
// the (field-identical) Underlyings type, so each element is copied into an array
// allocated from @p arena. The copied string_views still point into the NOS payload
// buffer, which outlives the synchronous ER encode. @p arena and the NOS payload must
// both stay alive until the ER has been encoded.
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

void echo_underlyings(const pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::UnderlyingsView>& source, pubsub_itc_fw::BumpAllocator& arena,
                      pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::Underlyings>& destination) {
    if (source.size == 0) {
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::Underlyings>(source.size);
    if (elements == nullptr) {
        return; // arena exhausted: leave the ER group empty rather than emit a partial one
    }
    for (size_t index = 0; index < source.size; ++index) {
        const pubsub_itc_fw_app::UnderlyingsView& in = source.data[index];
        pubsub_itc_fw_app::Underlyings& out = elements[index];
        out = pubsub_itc_fw_app::Underlyings{};
        out.has_underlying_symbol = in.has_underlying_symbol;
        out.underlying_symbol = in.underlying_symbol;
        out.has_underlying_security_id = in.has_underlying_security_id;
        out.underlying_security_id = in.underlying_security_id;
        out.has_underlying_qty = in.has_underlying_qty;
        out.underlying_qty = in.underlying_qty;
    }
    destination.data = elements;
    destination.size = source.size;
}

// Echo the NoPartyIDs group (with its nested NoPartySubIDs) from the decoded
// NewOrderSingle onto its ExecutionReport, mirroring echo_underlyings. PartyIDs carries
// a nested list, so the copy descends one level; all element arrays come from @p arena
// and the string_views point into the NOS payload -- both must outlive the ER encode.
void echo_parties(const pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDsView>& source, pubsub_itc_fw::BumpAllocator& arena,
                  pubsub_itc_fw_app::ListView<pubsub_itc_fw_app::PartyIDs>& destination) {
    if (source.size == 0) {
        return;
    }
    auto* elements = arena.allocate<pubsub_itc_fw_app::PartyIDs>(source.size);
    if (elements == nullptr) {
        return; // arena exhausted: leave the ER group empty rather than emit a partial one
    }
    for (size_t index = 0; index < source.size; ++index) {
        const pubsub_itc_fw_app::PartyIDsView& in = source.data[index];
        pubsub_itc_fw_app::PartyIDs& out = elements[index];
        out = pubsub_itc_fw_app::PartyIDs{};
        out.has_party_id = in.has_party_id;
        out.party_id = in.party_id;
        out.has_party_id_source = in.has_party_id_source;
        out.party_id_source = in.party_id_source;
        out.has_party_role = in.has_party_role;
        out.party_role = in.party_role;
        if (in.no_party_sub_i_ds.size == 0) {
            continue;
        }
        auto* sub_elements = arena.allocate<pubsub_itc_fw_app::PartySubIDs>(in.no_party_sub_i_ds.size);
        if (sub_elements == nullptr) {
            continue; // arena exhausted: emit the party without its sub-ids
        }
        for (size_t sub = 0; sub < in.no_party_sub_i_ds.size; ++sub) {
            const pubsub_itc_fw_app::PartySubIDsView& sub_in = in.no_party_sub_i_ds.data[sub];
            pubsub_itc_fw_app::PartySubIDs& sub_out = sub_elements[sub];
            sub_out = pubsub_itc_fw_app::PartySubIDs{};
            sub_out.has_party_sub_id = sub_in.has_party_sub_id;
            sub_out.party_sub_id = sub_in.party_sub_id;
            sub_out.has_party_sub_id_type = sub_in.has_party_sub_id_type;
            sub_out.party_sub_id_type = sub_in.party_sub_id_type;
        }
        out.no_party_sub_i_ds.data = sub_elements;
        out.no_party_sub_i_ds.size = in.no_party_sub_i_ds.size;
    }
    destination.data = elements;
    destination.size = source.size;
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
    , sequencer_er_conn_id_{}
    , sequencer_er_secondary_conn_id_{}
    , order_book_(0, OrderKeyHash{}, std::equal_to<OrderKey>{}, &book_growth_reporter_) {
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

    order_book_.reserve(static_cast<size_t>(config_.order_book_initial_capacity));

    // Registered unconditionally: this component has exactly one application thread and
    // names its scope above, so there is no second thread to collide with. The handle is a
    // no-op when metrics are disabled, and the application and component tokens come from
    // configuration rather than from here. See docs/design/metrics.md.
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: HA enabled, starting as {} (instance_id={}, configured {})",
                   me_role_name(ha_role_state_), config_.instance_id, is_primary_ ? "primary" : "secondary");
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: HA disabled -- single instance, no arbitration and no peer");
    }
}

void MatchingEngineThread::on_app_ready_event() {
    // Both roles connect outbound to the sequencer ER listeners (pre-warmed so
    // ERs, including the cancel-on-failover burst, flow immediately on promotion).
    connect_to_service("sequencer_er");
    connect_to_service("sequencer_er_secondary");

    if (is_primary_ && ha_enabled_) {
        connect_to_service("me_secondary_replication");
    }

    if (ha_enabled_) {
        // Both roles connect to the arbiter pool: the primary heartbeats to hold
        // its lease; the secondary requests arbitration on primary loss.
        connect_to_service("arbiter_primary");
        connect_to_service("arbiter_secondary");
    }
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& svc = id.service_name();
    const std::string order_inbound_svc = "inbound:" + std::to_string(config_.listen_port);
    const std::string replication_inbound_svc = "inbound:" + std::to_string(config_.replication_listen_port);

    if (svc == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: primary sequencer ER connection {} established", id.get_value());
        announce_role();
    } else if (svc == "sequencer_er_secondary") {
        sequencer_er_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
        announce_role();
    } else if (svc == "me_secondary_replication") {
        // Primary: outbound connection to ME-secondary's replication listener established.
        secondary_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: ME-secondary replication connection {} established", id.get_value());
    } else if (svc == "arbiter_primary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-primary connection {} established", id.get_value());
        if (first_arbiter) {
            start_arbiter_heartbeats();
        }
        if (first_arbiter && is_primary_) {
            request_startup_arbitration();
        }
    } else if (svc == "arbiter_secondary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-secondary connection {} established", id.get_value());
        if (first_arbiter) {
            start_arbiter_heartbeats();
        }
        if (first_arbiter && is_primary_) {
            request_startup_arbitration();
        }
    } else if (!is_primary_ && ha_enabled_ && svc == replication_inbound_svc) {
        // Secondary: inbound connection from ME-primary (book replication channel).
        primary_replication_conn_id_ = id;
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: ME-primary replication connection {} established -- replica book will be updated", id.get_value());
        // Slice C: if a promotion was pending (primary had dropped and we armed
        // the timeout), the primary has reconnected first -- cancel the promotion.
        if (promotion_pending_) {
            cancel_timer(promotion_timeout_timer_id_);
            promotion_pending_ = false;
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                           "MatchingEngineThread: primary reconnected before promotion -- timer cancelled, staying FOLLOWER");
        }
    } else if (!is_primary_ && ha_enabled_ && svc == order_inbound_svc) {
        // Secondary: inbound order connection from the sequencer. While in FOLLOWER
        // mode this is idle (order PDUs are discarded). If we are already RECONCILING
        // when the sequencer connects, begin WAL catch-up immediately.
        sequencer_order_conn_ids_.insert(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer order connection {} established (state={})",
                   id.get_value(), static_cast<int>(ha_role_state_));
        if (ha_role_state_ == MeRole::Reconciling) {
            begin_reconciliation();
        }
    } else {
        // Primary (or non-HA) inbound order connection from the sequencer.
        sequencer_order_conn_ids_.insert(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound sequencer order connection {} established", id.get_value());
    }
}

void MatchingEngineThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: primary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == sequencer_er_secondary_conn_id_) {
        sequencer_er_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: secondary sequencer ER connection {} lost: {}", id.get_value(),
                   reason);
    } else if (id == secondary_replication_conn_id_) {
        secondary_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-secondary replication connection {} lost: {} -- book updates paused until secondary reconnects", id.get_value(),
                   reason);
    } else if (id == primary_replication_conn_id_) {
        primary_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-primary replication connection {} lost: {} -- replica book is now stale (last seq={})", id.get_value(), reason,
                   last_replicated_seq_no_);
        // Slice C: primary loss on the follower triggers arbiter-mediated promotion.
        // Arm the promotion timer; if the primary reconnects before it fires we cancel it.
        if (ha_role_state_ == MeRole::Follower) {
            promotion_timeout_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
            promotion_pending_ = true;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: promotion timeout armed ({}s) -- will request arbitration if primary does not reconnect",
                       config_.heartbeat_timeout_seconds);
        }
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer(arbiter_heartbeat_timer_id_);
        }
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer(arbiter_heartbeat_timer_id_);
        }
    } else if (sequencer_order_conn_ids_.count(id) > 0) {
        sequencer_order_conn_ids_.erase(id);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound sequencer order connection {} lost: {}", id.get_value(),
                   reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: inbound connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEngineThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: sequenced PDU received on connection {} pdu_id={}",
               message.connection_id().get_value(), pdu_id);

    // ---- HA control PDUs (Slice C+D) --------------------------------------
    if (pdu_id == pubsub_itc_fw_app::ArbitrationDecision::message_pdu_id) {
        handle_arbitration_decision(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::MePositionAck::message_pdu_id) {
        handle_me_position_ack(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pubsub_itc_fw_app::Heartbeat::message_pdu_id) {
        // Heartbeat echoes from the arbiter -- liveness only, nothing to do.
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
    const bool is_order_pdu = (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) ||
                              (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest));

    // ---- Order PDU gating by HA state -------------------------------------
    if (is_order_pdu && ha_role_state_ == MeRole::Follower) {
        // Passive follower: order PDUs from the sequencer are discarded (the primary
        // is authoritative; the follower's book is maintained via BookUpdate replication).
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: FOLLOWER -- discarding order PDU inner_pdu_id={} on connection {}",
                   inner_pdu_id, message.connection_id().get_value());
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
        handle_new_order_single(view, message.seq_no(), envelope.wall_time_ns, session_identity_from(envelope));

    } else if (inner_pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        pubsub_itc_fw_app::OrderCancelRequestView view{};
        if (!pubsub_itc_fw_app::decode(view, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode OrderCancelRequest -- dropping");
            release_pdu_payload(message);
            return;
        }
        handle_order_cancel_request(view, message.seq_no(), envelope.wall_time_ns, session_identity_from(envelope));

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

void MatchingEngineThread::handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number, int64_t sequenced_at_ns,
                                                   const fix_common::SessionIdentity& session) {
    // RECONCILING (Slice D): apply the WAL catch-up NOS to the book but do NOT
    // emit an ER and do NOT replicate. The gateway already saw ERs for these
    // orders from the failed primary; re-sending would duplicate them.
    if (ha_role_state_ == MeRole::Reconciling) {
        const OrderKey recon_key = OrderKey::make(session, view.cl_ord_id);
        if (order_book_.count(recon_key)) {
            return; // duplicate during replay -- ignore
        }
        OrderEntry recon_entry{};
        recon_entry.order_id_num = ++order_id_counter_;
        recon_entry.side = view.side;
        recon_entry.has_price = view.has_price;
        recon_entry.ord_type = view.ord_type;
        recon_entry.set_symbol(view.symbol);
        recon_entry.set_order_qty(view.order_qty);
        if (view.has_price) {
            recon_entry.set_price(view.price);
        }
        recon_entry.session = session;
        order_book_.emplace(recon_key, recon_entry);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: RECONCILING apply NOS seq={} cl_ord_id={} book_size={}",
                   sequence_number, view.cl_ord_id, order_book_.size());
        return;
    }

    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: no sequencer ER connections established -- dropping NOS");
        return;
    }

    const int64_t now_ns = sequenced_at_ns != 0 ? sequenced_at_ns : config_.wall_clock->now_ns();
    const OrderKey order_key = OrderKey::make(session, view.cl_ord_id);

    // Stack-allocated ID buffers -- no heap allocation.
    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

    // Reject duplicate ClOrdID within the same FIX session.
    if (order_book_.count(order_key)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: duplicate ClOrdID={} (session comp_id='{}') -- rejecting NOS",
                   view.cl_ord_id, session.comp_id_view());

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = "NONE";
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol = view.symbol;
        er.side = view.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = now_ns;
        er.has_cl_ord_id = true;
        er.cl_ord_id = view.cl_ord_id;
        er.has_order_qty = true;
        er.order_qty = view.order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::DuplicateOrder;

        send_er_to_sequencer(er, sequence_number);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sent rejection ER ExecID={} ClOrdID={} (DuplicateOrder)", exec_id,
                   view.cl_ord_id);
        return;
    }

    // Accept the order: add to book and acknowledge with ExecType=New.
    std::array<char, 32> order_id_buf{};
    const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, ++order_id_counter_);

    OrderEntry entry{};
    entry.order_id_num = order_id_counter_;
    entry.session = session;
    entry.side = view.side;
    entry.has_price = view.has_price;
    entry.ord_type = view.ord_type;
    entry.has_time_in_force = view.has_time_in_force;
    entry.time_in_force = view.time_in_force;
    // Stored exactly as sent -- no rounding to a session close, no trading-calendar
    // resolution, no clamp to a maximum lifetime. See OrderEntry::expire_time for why this
    // is a decision rather than merely the absence of one.
    entry.has_expire_time = view.has_expire_time;
    entry.expire_time = view.expire_time;
    entry.set_symbol(view.symbol);
    entry.set_order_qty(view.order_qty);
    if (view.has_price) {
        entry.set_price(view.price);
    }
    order_book_.emplace(order_key, entry);

    // Counted here, on the one path that puts an order on the book. Not at entry to this
    // function: the reconciliation and follower paths return before this point, and a
    // counter that jumped by the whole replayed backlog at every failover would be
    // unusable as an order rate -- which is the thing it is for. The duplicate-ClOrdID
    // rejection above is likewise not an order processed.
    orders_processed_counter_.increment();

    // Replicate the new entry to ME-secondary.
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Add, session, view.cl_ord_id, &entry);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = order_id;
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::New;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::New;
    er.symbol = view.symbol;
    er.side = view.side;
    er.leaves_qty = view.order_qty;
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = now_ns;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = view.order_qty;
    er.has_ord_type = true;
    er.ord_type = view.ord_type;
    // Echoed so the gateway can tell a persistent order from a day order without holding
    // the original NOS. Cancel-on-disconnect exempts GoodTillCancel and GoodTillDate, and
    // the ER is the only thing the gateway sees for an order resting on the book.
    if (view.has_expire_time) {
        // The member's confirmation of when its order dies -- one of the terms it is
        // trading on, and the only way it can tell which expiry convention this venue
        // follows. (The FIX rendering on the wire is second-precision, matching what the
        // gateway's inbound parser reads; that is a property of the protocol hop rather
        // than an adjustment to the value.)
        er.has_expire_time = true;
        er.expire_time = view.expire_time;
    }
    if (view.has_time_in_force) {
        er.has_time_in_force = true;
        er.time_in_force = view.time_in_force;
    }
    if (view.has_price) {
        er.has_price = true;
        er.price = view.price;
    }

    // Echo the order's NoUnderlyings and NoPartyIDs groups back on the acknowledgement so
    // downstream topic subscribers see the instrument legs and parties the client sent. The
    // element arrays live in the reusable er_group_arena_buffer_ (string_views point into
    // the still-live NOS payload); send_er_to_sequencer encodes synchronously below. The
    // arena is sized to need, not a fixed cap: echo, and if it was too small (bytes_used
    // exceeds it) grow to the requirement and retry, so large group sets are never silently
    // dropped. bytes_used reports the true need even when an allocation was refused.
    for (;;) {
        pubsub_itc_fw::BumpAllocator group_arena(er_group_arena_buffer_.data(), er_group_arena_buffer_.size());
        er.no_underlyings = {};
        er.no_party_i_ds = {};
        echo_underlyings(view.no_underlyings, group_arena, er.no_underlyings);
        echo_parties(view.no_party_i_ds, group_arena, er.no_party_i_ds);
        if (group_arena.bytes_used() <= er_group_arena_buffer_.size() || er_group_arena_buffer_.size() >= max_er_group_arena_size) {
            break;
        }
        er_group_arena_buffer_.resize(std::max(group_arena.bytes_used(), er_group_arena_buffer_.size() * 2));
    }

    send_er_to_sequencer(er, sequence_number);
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it
    // and the test breaks, silently and elsewhere.
    //
    // Kept at Info deliberately, having been tried at Debug and reverted. It costs 226
    // bytes per order -- 9.5 GB a day at 40 million orders -- but that volume only arises
    // under load, and ha_test's scenarios are a few thousand orders. Its HA assertions
    // read ME-ORD-N, the ME's own order_id_counter_, which advances during WAL
    // reconciliation where orders_processed_total deliberately does not; the two diverge
    // after a failover, so the counter cannot express "no ME-ORD gap or reset".
    //
    // A LOAD RUN raises this component's applog_level instead, which is per-component
    // configuration and needs no code change. See perf_run.py --profile.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: accepted NOS OrderID={} ExecID={} ClOrdID={} book_size={}", order_id,
               exec_id, view.cl_ord_id, order_book_.size());
}

void MatchingEngineThread::handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number, int64_t sequenced_at_ns,
                                                       const fix_common::SessionIdentity& session) {
    // RECONCILING (Slice D): apply the WAL catch-up OCR to the book but do NOT emit an ER.
    if (ha_role_state_ == MeRole::Reconciling) {
        const OrderKey recon_key = OrderKey::make(session, view.orig_cl_ord_id);
        order_book_.erase(recon_key);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: RECONCILING apply OCR seq={} orig_cl_ord_id={} book_size={}",
                   sequence_number, view.orig_cl_ord_id, order_book_.size());
        return;
    }

    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: no sequencer ER connections established -- dropping OCR");
        return;
    }

    const int64_t now_ns = sequenced_at_ns != 0 ? sequenced_at_ns : config_.wall_clock->now_ns();
    const OrderKey orig_key = OrderKey::make(session, view.orig_cl_ord_id);

    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: OrderCancelRequest seq={} ClOrdID={} OrigClOrdID={} Symbol={} Side={}",
               sequence_number, view.cl_ord_id, view.orig_cl_ord_id, view.symbol, static_cast<char>(view.side));

    auto it = order_book_.find(orig_key);
    if (it == order_book_.end()) {
        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = "NONE";
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol = view.symbol;
        er.side = view.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = now_ns;
        er.has_cl_ord_id = true;
        er.cl_ord_id = view.cl_ord_id;
        er.has_orig_cl_ord_id = true;
        er.orig_cl_ord_id = view.orig_cl_ord_id;
        er.has_order_qty = true;
        er.order_qty = view.order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::UnknownOrder;

        send_er_to_sequencer(er, sequence_number);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: sent rejection ER ExecID={} OrigClOrdID={} (UnknownOrder)", exec_id,
                   view.orig_cl_ord_id);
        return;
    }

    // Order found -- cancel it.
    const OrderEntry entry = it->second;
    order_book_.erase(it);

    // Replicate the removal to ME-secondary.
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Remove, session, view.orig_cl_ord_id, nullptr);

    // Format order_id from the stored counter value.
    std::array<char, 32> order_id_buf{};
    const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, entry.order_id_num);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = order_id;
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
    er.symbol = entry.get_symbol();
    er.side = entry.side;
    // A report describes the order it concerns, and a cancel is no exception: a member
    // reconciling a cancel it did not initiate -- one arriving on a reconnect, say -- has
    // only the report to tell it which order this was.
    if (entry.has_time_in_force) {
        er.has_time_in_force = true;
        er.time_in_force = entry.time_in_force;
    }
    if (entry.has_expire_time) {
        er.has_expire_time = true;
        er.expire_time = entry.expire_time;
    }
    er.leaves_qty = "0";
    er.cum_qty = "0";
    er.avg_px = "0.00";
    er.transact_time = now_ns;
    er.has_cl_ord_id = true;
    er.cl_ord_id = view.cl_ord_id;
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id = view.orig_cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = entry.get_order_qty();
    if (entry.has_price) {
        er.has_price = true;
        er.price = entry.get_price();
    }
    er.has_ord_type = true;
    er.ord_type = entry.ord_type;

    send_er_to_sequencer(er, sequence_number);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: sent cancel ER OrderID={} ExecID={} ClOrdID={} OrigClOrdID={} book_size={}", order_id, exec_id, view.cl_ord_id,
               view.orig_cl_ord_id, order_book_.size());
}

void MatchingEngineThread::send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no, const fix_common::SessionIdentity& session) {
    // Encode the ER, then wrap it in a WalRecord envelope. The echoed seq_no travels in
    // the transport header, which is how the sequencer routes an ordinary ER: it looks the
    // order's sequence up and finds the session that placed it. The session identity on
    // the envelope is for the ERs that have no such sequence -- the seq_no==0
    // cancel-on-failover ones -- and says whose order was cancelled without saying where
    // that member is, which the ME cannot know and which by then has usually changed.
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

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = seq_no;
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    envelope.payload.data = er_encode_buffer_.data();
    envelope.payload.size = bytes_written;
    envelope.has_sender_comp_id = !session.empty();
    envelope.sender_comp_id = session.comp_id_view();
    envelope.has_origin_gateway_id = !session.empty();
    envelope.origin_gateway_id = session.protocol;

    if (sequencer_er_conn_id_.is_valid()) {
        send_pdu(sequencer_er_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq_no, envelope);
    }
    if (sequencer_er_secondary_conn_id_.is_valid()) {
        send_pdu(sequencer_er_secondary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, seq_no, envelope);
    }
}

void MatchingEngineThread::on_timer_event(pubsub_itc_fw::TimerID id) {
    if (id == startup_arbitration_timer_id_) {
        if (ha_role_state_ == MeRole::Unknown) {
            // Silence from a connected arbiter is not absence. An arbiter that has itself just
            // restarted declines to answer until it knows who leads, precisely so that it does
            // not guess -- and self-promoting against that decision would produce the second
            // leader the decline exists to prevent. So ask again, and only give up after
            // enough attempts that the arbiter is evidently not going to answer at all.
            ++startup_arbitration_attempts_;
            if (startup_arbitration_attempts_ < max_startup_arbitration_attempts) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "MatchingEngineThread: no ArbitrationDecision within {}s (attempt {} of {}) -- asking again", config_.heartbeat_timeout_seconds,
                           startup_arbitration_attempts_, max_startup_arbitration_attempts);
                send_arbitration_report();
                startup_arbitration_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
                return;
            }
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: no ArbitrationDecision after {} attempts -- self-promoting via instance-id rule (degraded)",
                       startup_arbitration_attempts_);
            ++epoch_;
            begin_reconciliation();
        }
        return;
    }

    if (id == promotion_timeout_timer_id_) {
        // Slice C: the primary did not reconnect in time. Request arbitration.
        promotion_pending_ = false;
        if (ha_role_state_ != MeRole::Follower) {
            return; // state changed (e.g. primary reconnected) -- nothing to do
        }
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: promotion timeout fired -- requesting arbitration from arbiter pool");
        send_arbitration_report();
        return;
    }

    if (id == arbiter_heartbeat_timer_id_) {
        send_arbiter_heartbeat();
        return;
    }
}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void MatchingEngineThread::send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, const fix_common::SessionIdentity& session,
                                            std::string_view cl_ord_id, const OrderEntry* entry) {
    if (!ha_enabled_ || !is_primary_ || !secondary_replication_conn_id_.is_valid()) {
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
        upd.order_id_num = entry->order_id_num;
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

    send_pdu(secondary_replication_conn_id_, pubsub_itc_fw_app::BookUpdate::message_pdu_id, seq_no, upd);
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
        entry.order_id_num = view.order_id_num;
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
        order_book_.insert_or_assign(key, entry);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: replica Add seq={} cl_ord_id={} book_size={}", view.seq_no,
                   view.cl_ord_id, order_book_.size());
    } else if (update_type == pubsub_itc_fw_app::BookUpdateType::Remove) {
        order_book_.erase(key);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: replica Remove seq={} cl_ord_id={} book_size={}", view.seq_no,
                   view.cl_ord_id, order_book_.size());
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

void MatchingEngineThread::request_startup_arbitration() {
    // Only from Unknown. A role already settled -- by an earlier decision, or by the
    // reconciliation a promotion starts -- must not be reopened because a second arbiter
    // connection happened to come up.
    if (ha_role_state_ != MeRole::Unknown) {
        return;
    }
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: asking the arbiter which instance leads before adopting any role");
    send_arbitration_report();

    // send_arbitration_report() self-promotes outright when no arbiter is connected at all.
    // This covers the other silence: an arbiter is there and does not answer, which would
    // otherwise leave the venue with no matching engine leader and nothing to say so.
    if (ha_role_state_ == MeRole::Unknown) {
        cancel_timer(startup_arbitration_timer_id_);
        startup_arbitration_timer_id_ = start_one_off_timer(std::chrono::seconds(config_.heartbeat_timeout_seconds));
    }
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
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: adopting LEADER role (epoch={})", epoch_);
    ha_role_state_ = MeRole::Leader;
    // The timer already runs -- it is started when the arbiter connection comes up, in every
    // role. Send one now so the lease is asserted immediately rather than at the next tick.
    send_arbiter_heartbeat();
    announce_role();
}

void MatchingEngineThread::send_arbitration_report() {
    pubsub_itc_fw_app::ArbitrationReport report{};
    report.self_instance_id = static_cast<int64_t>(config_.instance_id);
    report.peer_instance_id = peer_instance_id();
    report.epoch = epoch_;
    report.proposed_role = pubsub_itc_fw_app::Role::leader;
    report.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pubsub_itc_fw_app::ArbitrationReport::message_pdu_id, 0, report);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pubsub_itc_fw_app::ArbitrationReport::message_pdu_id, 0, report);
    }
    if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
        // No arbiter reachable -- degrade to the local instance-id rule and self-promote.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: no arbiter connected -- self-promoting via instance-id rule (degraded)");
        ++epoch_;
        begin_reconciliation();
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: ArbitrationReport sent (self_instance_id={} peer_instance_id={} epoch={})",
               report.self_instance_id, report.peer_instance_id, report.epoch);
}

void MatchingEngineThread::handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::ArbitrationDecisionView decision{};

    if (!pubsub_itc_fw_app::decode(decision, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode ArbitrationDecision -- dropping");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: ArbitrationDecision received (group={} leader={} follower={} epoch={})",
               pubsub_itc_fw_app::to_string(decision.group), decision.leader_instance_id, decision.follower_instance_id, decision.epoch);

    // Defence in depth: reject any decision not addressed to the matching_engine
    // group so a routing mistake can never drive a spurious ME promotion.
    if (decision.group != pubsub_itc_fw_app::ComponentGroup::matching_engine) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ArbitrationDecision addressed to group={} (not matching_engine) -- ignoring",
                   pubsub_itc_fw_app::to_string(decision.group));
        return;
    }

    // The ME sends ArbitrationReport to both arbiters, so both may reply. Ignore a
    // duplicate decision once we are already promoting (Reconciling) or promoted
    // (Leader): re-running reconciliation would wrongly cancel orders accepted
    // after the first promotion completed.
    // A decision carrying an epoch we have already reached is the second arbiter's copy of one
    // we have acted on, and re-running reconciliation on it would wrongly cancel orders
    // accepted since. A NEWER epoch is not a duplicate whatever role we hold: it is the
    // arbiter telling us the answer has changed, and discarding it is how an instance that
    // promoted itself stays wrong. That is not hypothetical -- it is the bug this check used
    // to cause; see docs/bug_list.md, "A restarted primary matching engine promotes itself".
    const bool already_settled = ha_role_state_ == MeRole::Reconciling || ha_role_state_ == MeRole::Leader;
    if (already_settled && decision.epoch <= epoch_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: already {} at epoch {} -- ignoring duplicate ArbitrationDecision (epoch={})",
                   ha_role_state_ == MeRole::Leader ? "LEADER" : "RECONCILING", epoch_, decision.epoch);
        return;
    }

    cancel_timer(startup_arbitration_timer_id_);
    epoch_ = decision.epoch;

    if (decision.leader_instance_id == static_cast<int64_t>(config_.instance_id)) {
        if (ha_role_state_ == MeRole::Follower) {
            // Taking over from a peer that has been serving: catch up on the WAL before
            // accepting anything, or this instance leads a book it does not have.
            begin_reconciliation();
        } else {
            // Nothing to take over. This is a start rather than a promotion -- the peer is
            // not leading and no replica book has been maintained here -- and reconciliation
            // has no connection to wait on, so entering it strands the venue without a
            // matching engine leader. See docs/bug_list.md, "A cold-start primary that is
            // told it leads should not reconcile".
            adopt_leader_role();
        }
    } else if (decision.follower_instance_id == static_cast<int64_t>(config_.instance_id)) {
        // We remain the follower: stay passive.
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter assigned follower role -- staying passive");
        enter_follower_state();
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ArbitrationDecision does not mention this instance (instance_id={}) -- ignoring", config_.instance_id);
    }
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
    }
    // The timer is deliberately left running. A follower still sends liveness; what it stops
    // sending is the lease, which send_arbiter_heartbeat decides by role.
    announce_role();
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

    for (const pubsub_itc_fw::ConnectionID& conn : {sequencer_er_conn_id_, sequencer_er_secondary_conn_id_}) {
        if (conn.is_valid()) {
            send_pdu(conn, pubsub_itc_fw_app::RoleAnnouncement::message_pdu_id, 0, announcement);
        }
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: announced role {} at epoch {} to the sequencers",
               pubsub_itc_fw_app::to_string(announcement.current_role), epoch_);
}

void MatchingEngineThread::start_arbiter_heartbeats() {
    cancel_timer(arbiter_heartbeat_timer_id_);
    arbiter_heartbeat_timer_id_ = start_recurring_timer(std::chrono::seconds(config_.heartbeat_interval_seconds));
    // One immediately, so the arbiter registers this instance without waiting an interval.
    send_arbiter_heartbeat();
}

void MatchingEngineThread::send_arbiter_heartbeat() {
    // Liveness only, and sent whatever role this instance holds. The arbiter registers a
    // component when it hears from it, and it needs to know a follower is there as well as a
    // leader -- its own cold-start rule asks whether the peer is connected.
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    hb.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    for (const pubsub_itc_fw::ConnectionID& conn : {arbiter_primary_conn_id_, arbiter_secondary_conn_id_}) {
        if (conn.is_valid()) {
            send_pdu(conn, pubsub_itc_fw_app::Heartbeat::message_pdu_id, 0, hb);
        }
    }

    // Leadership is a separate assertion, made only by whoever holds it. Keeping it out of
    // the heartbeat is what lets a follower send one at all.
    if (ha_role_state_ == MeRole::Leader) {
        pubsub_itc_fw_app::LeadershipLease lease{};
        lease.instance_id = static_cast<int64_t>(config_.instance_id);
        lease.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
        lease.epoch = epoch_;
        for (const pubsub_itc_fw::ConnectionID& conn : {arbiter_primary_conn_id_, arbiter_secondary_conn_id_}) {
            if (conn.is_valid()) {
                send_pdu(conn, pubsub_itc_fw_app::LeadershipLease::message_pdu_id, 0, lease);
            }
        }
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: arbiter heartbeat sent (instance_id={} epoch={} leader={})",
               hb.instance_id, hb.epoch, ha_role_state_ == MeRole::Leader);
}

// WAL reconciliation (Slice D)

void MatchingEngineThread::begin_reconciliation() {
    // Cancel the promotion timer (it fired or arbitration is complete).
    cancel_timer(promotion_timeout_timer_id_);
    ha_role_state_ = MeRole::Reconciling;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: entering RECONCILING (last_replicated_seq_no={}, book_size={})",
               last_replicated_seq_no_, order_book_.size());

    // The sequencers connect to our (now-listening) order port. If at least one is
    // already connected, request WAL catch-up immediately; otherwise
    // on_connection_established will call begin_reconciliation() again once a
    // connection is up.
    if (!sequencer_order_conn_ids_.empty()) {
        send_me_position_request();
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                       "MatchingEngineThread: RECONCILING -- awaiting sequencer order connection before WAL catch-up");
    }
}

void MatchingEngineThread::send_me_position_request() {
    if (sequencer_order_conn_ids_.empty()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: cannot send MePositionRequest -- no sequencer order connection");
        return;
    }
    // Send to every sequencer order connection. Only the leader sequencer serves
    // WAL catch-up and acks; followers re-point their own order connection without
    // streaming, so the promoted ME receives exactly one catch-up stream and one
    // ack regardless of which pre-warmed connection the current leader owns.
    pubsub_itc_fw_app::MePositionRequest request{};
    request.last_seq_no = last_replicated_seq_no_;
    for (const auto& conn_id : sequencer_order_conn_ids_) {
        send_pdu(conn_id, pubsub_itc_fw_app::MePositionRequest::message_pdu_id, 0, request);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: MePositionRequest sent to connection {} (last_seq_no={})",
                   conn_id.get_value(), last_replicated_seq_no_);
    }
}

void MatchingEngineThread::handle_me_position_ack(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::MePositionAckView ack{};

    if (!pubsub_itc_fw_app::decode(ack, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode MePositionAck -- dropping");
        return;
    }

    // Reconcile on the first ack only. We fan MePositionRequest out to every
    // sequencer order connection, so a straggler ack (e.g. from a sequencer that
    // just won an election) can still arrive after we have promoted; ignore it
    // rather than re-running cancel-on-failover.
    if (ha_role_state_ != MeRole::Reconciling) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: already promoted (state={}) -- ignoring duplicate MePositionAck",
                   static_cast<int>(ha_role_state_));
        return;
    }

    last_replicated_seq_no_ = ack.last_seq_no;
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: MePositionAck received -- book reconciled to seq_no={} (book_size={})",
               ack.last_seq_no, order_book_.size());

    // Cancel-on-failover: cancel every live order before resuming as leader.
    cancel_all_orders_on_failover();

    // Transition to LEADER -- normal processing begins.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: reconciliation complete at seq_no={} with {} order(s) on the book -- resuming as leader", ack.last_seq_no,
               order_book_.size());
    ha_role_state_ = MeRole::Unknown; // clear reconciling so adopt_leader_role proceeds
    adopt_leader_role();
}

void MatchingEngineThread::cancel_all_orders_on_failover() {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: cancel-on-failover -- cancelling {} live order(s)", order_book_.size());

    const int64_t now_ns = config_.wall_clock->now_ns();
    size_t cancelled = 0;

    for (const auto& kv : order_book_) {
        const OrderKey& key = kv.first;
        const OrderEntry& entry = kv.second;

        std::array<char, 32> exec_id_buf{};
        const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);
        std::array<char, 32> order_id_buf{};
        const std::string_view order_id = format_id(order_id_buf, "ME-ORD-", 7, entry.order_id_num);
        const std::string_view cl_ord_id{key.cl_ord_id.data(), key.cl_ord_id_len};

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id = order_id;
        er.exec_id = exec_id;
        er.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
        er.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
        er.symbol = entry.get_symbol();
        er.side = entry.side;
        er.leaves_qty = "0";
        er.cum_qty = "0";
        er.avg_px = "0.00";
        er.transact_time = now_ns;
        er.has_cl_ord_id = true;
        er.cl_ord_id = cl_ord_id;
        er.has_order_qty = true;
        er.order_qty = entry.get_order_qty();
        if (entry.has_price) {
            er.has_price = true;
            er.price = entry.get_price();
        }
        er.has_ord_type = true;
        er.ord_type = entry.ord_type;

        // seq_no 0: these cancels are generated by the ME on promotion, not driven by a
        // sequenced order, so the sequencer cannot route them via its seq->conn map. The
        // originating session's connection id rides on the envelope instead.
        send_er_to_sequencer(er, 0, entry.session);
        ++cancelled;
    }

    order_book_.clear();
    // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: cancel-on-failover complete -- {} cancel ER(s) sent, book cleared",
               cancelled);
}

} // namespaces
