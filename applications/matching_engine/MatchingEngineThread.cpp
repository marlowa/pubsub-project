// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

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
constexpr int16_t pdu_book_update = 600;

// Leader-follower protocol PDU ids (must match leader_follower.dsl).
constexpr int16_t pdu_heartbeat = 102;
constexpr int16_t pdu_me_position_request = 115;
constexpr int16_t pdu_me_position_ack = 116;
constexpr int16_t pdu_arbitration_report = 200;
constexpr int16_t pdu_arbitration_decision = 201;

// Timer names.
constexpr const char* timer_promotion_timeout = "me_promotion_timeout";
constexpr const char* timer_arbiter_heartbeat = "me_arbiter_heartbeat";

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
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
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , ha_enabled_(config.ha_enabled)
    , is_primary_(!config.ha_enabled || config.ha_role == "primary")
    , sequencer_er_conn_id_{}
    , sequencer_er_secondary_conn_id_{} {
    order_book_.reserve(static_cast<size_t>(config_.order_book_initial_capacity));

    // HA classification. A non-HA ME is a plain single instance (Unknown state,
    // full processing). The HA primary begins Unknown and adopts Leader once it
    // has connected to the arbiter (holding its lease). The HA secondary begins
    // as a passive Follower.
    if (ha_enabled_) {
        ha_role_state_ = is_primary_ ? MeRole::Unknown : MeRole::Follower;
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
    } else if (svc == "sequencer_er_secondary") {
        sequencer_er_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
    } else if (svc == "me_secondary_replication") {
        // Primary: outbound connection to ME-secondary's replication listener established.
        secondary_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: ME-secondary replication connection {} established", id.get_value());
    } else if (svc == "arbiter_primary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-primary connection {} established", id.get_value());
        if (first_arbiter && is_primary_) {
            // Primary holds leadership by heartbeating the arbiter to renew its lease.
            adopt_leader_role();
        }
    } else if (svc == "arbiter_secondary") {
        const bool first_arbiter = !arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid();
        arbiter_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: arbiter-secondary connection {} established", id.get_value());
        if (first_arbiter && is_primary_) {
            adopt_leader_role();
        }
    } else if (!is_primary_ && ha_enabled_ && svc == replication_inbound_svc) {
        // Secondary: inbound connection from ME-primary (book replication channel).
        primary_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: ME-primary replication connection {} established -- replica book will be updated", id.get_value());
        // Slice C: if a promotion was pending (primary had dropped and we armed
        // the timeout), the primary has reconnected first -- cancel the promotion.
        if (promotion_pending_) {
            cancel_timer(timer_promotion_timeout);
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
            start_one_off_timer(timer_promotion_timeout, std::chrono::seconds(config_.heartbeat_timeout_seconds));
            promotion_pending_ = true;
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: promotion timeout armed ({}s) -- will request arbitration if primary does not reconnect",
                       config_.heartbeat_timeout_seconds);
        }
    } else if (id == arbiter_primary_conn_id_) {
        arbiter_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-primary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer(timer_arbiter_heartbeat);
        }
    } else if (id == arbiter_secondary_conn_id_) {
        arbiter_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: arbiter-secondary connection {} lost: {}", id.get_value(), reason);
        if (!arbiter_primary_conn_id_.is_valid() && !arbiter_secondary_conn_id_.is_valid()) {
            cancel_timer(timer_arbiter_heartbeat);
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
    if (pdu_id == pdu_arbitration_decision) {
        handle_arbitration_decision(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pdu_me_position_ack) {
        handle_me_position_ack(message);
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == pdu_heartbeat) {
        // Heartbeat echoes from the arbiter -- liveness only, nothing to do.
        release_pdu_payload(message);
        return;
    }
    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport)) {
        // ERs may arrive on the ME's ER connections (the sequencer streams them).
        // The ME is the source of ERs, not a consumer -- discard.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: ExecutionReport received on connection {} -- discarding (ME does not consume ERs)",
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    // ---- Order PDU gating by HA state -------------------------------------
    const bool is_order_pdu = (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) ||
                              (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest));

    if (is_order_pdu && ha_role_state_ == MeRole::Follower) {
        // Passive follower: order PDUs from the sequencer are discarded (the primary
        // is authoritative; the follower's book is maintained via BookUpdate replication).
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: FOLLOWER -- discarding order PDU pdu_id={} on connection {}", pdu_id,
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::NewOrderSingleView view{};

        if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode NewOrderSingle -- dropping");
            release_pdu_payload(message);
            return;
        }
        handle_new_order_single(view, message.seq_no());

    } else if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        auto& arena_buf = decode_arena_buffer();
        pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
        arena.reset();
        size_t arena_bytes_needed = 0;
        size_t bytes_consumed = 0;
        pubsub_itc_fw_app::OrderCancelRequestView view{};

        if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to decode OrderCancelRequest -- dropping");
            release_pdu_payload(message);
            return;
        }
        handle_order_cancel_request(view, message.seq_no());

    } else if (pdu_id == pdu_book_update) {
        // BookUpdate from ME-primary -- secondary updates its replica book.
        apply_book_update(message);
        return; // apply_book_update calls release_pdu_payload internally.
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported sequenced PDU id {} -- dropping", pdu_id);
    }

    release_pdu_payload(message);
}

void MatchingEngineThread::handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number) {
    // RECONCILING (Slice D): apply the WAL catch-up NOS to the book but do NOT
    // emit an ER and do NOT replicate. The gateway already saw ERs for these
    // orders from the failed primary; re-sending would duplicate them.
    if (ha_role_state_ == MeRole::Reconciling) {
        const int32_t recon_session_id = view.has_gateway_session_conn_id ? view.gateway_session_conn_id : 0;
        const OrderKey recon_key = OrderKey::make(recon_session_id, view.cl_ord_id);
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
        recon_entry.gateway_session_conn_id = recon_session_id;
        order_book_.emplace(recon_key, recon_entry);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: RECONCILING apply NOS seq={} cl_ord_id={} book_size={}",
                   sequence_number, view.cl_ord_id, order_book_.size());
        return;
    }

    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: no sequencer ER connections established -- dropping NOS");
        return;
    }

    const int64_t now_ns = view.has_sequenced_at ? view.sequenced_at : config_.wall_clock->now_ns();
    const int32_t session_id = view.has_gateway_session_conn_id ? view.gateway_session_conn_id : 0;
    const OrderKey order_key = OrderKey::make(session_id, view.cl_ord_id);

    // Stack-allocated ID buffers -- no heap allocation.
    std::array<char, 32> exec_id_buf{};
    const std::string_view exec_id = format_id(exec_id_buf, "ME-EXEC-", 8, ++exec_id_counter_);

    // Reject duplicate ClOrdID within the same FIX session.
    if (order_book_.count(order_key)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: duplicate ClOrdID={} (session {}) -- rejecting NOS", view.cl_ord_id,
                   session_id);

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
    entry.gateway_session_conn_id = session_id;
    entry.side = view.side;
    entry.has_price = view.has_price;
    entry.ord_type = view.ord_type;
    entry.set_symbol(view.symbol);
    entry.set_order_qty(view.order_qty);
    if (view.has_price) {
        entry.set_price(view.price);
    }
    order_book_.emplace(order_key, entry);

    // Replicate the new entry to ME-secondary.
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Add, session_id, view.cl_ord_id, &entry);

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
    if (view.has_price) {
        er.has_price = true;
        er.price = view.price;
    }

    send_er_to_sequencer(er, sequence_number);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: accepted NOS OrderID={} ExecID={} ClOrdID={} book_size={}", order_id,
               exec_id, view.cl_ord_id, order_book_.size());
}

void MatchingEngineThread::handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number) {
    // RECONCILING (Slice D): apply the WAL catch-up OCR to the book but do NOT emit an ER.
    if (ha_role_state_ == MeRole::Reconciling) {
        const int32_t recon_session_id = view.has_gateway_session_conn_id ? view.gateway_session_conn_id : 0;
        const OrderKey recon_key = OrderKey::make(recon_session_id, view.orig_cl_ord_id);
        order_book_.erase(recon_key);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: RECONCILING apply OCR seq={} orig_cl_ord_id={} book_size={}",
                   sequence_number, view.orig_cl_ord_id, order_book_.size());
        return;
    }

    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: no sequencer ER connections established -- dropping OCR");
        return;
    }

    const int64_t now_ns = view.has_sequenced_at ? view.sequenced_at : config_.wall_clock->now_ns();
    const int32_t session_id = view.has_gateway_session_conn_id ? view.gateway_session_conn_id : 0;
    const OrderKey orig_key = OrderKey::make(session_id, view.orig_cl_ord_id);

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
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Remove, session_id, view.orig_cl_ord_id, nullptr);

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

void MatchingEngineThread::send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no) {
    constexpr auto er_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    if (sequencer_er_conn_id_.is_valid()) {
        send_pdu(sequencer_er_conn_id_, er_pdu_id, seq_no, er);
    }
    if (sequencer_er_secondary_conn_id_.is_valid()) {
        send_pdu(sequencer_er_secondary_conn_id_, er_pdu_id, seq_no, er);
    }
}

void MatchingEngineThread::on_timer_event(const std::string& name) {
    if (name == timer_promotion_timeout) {
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

    if (name == timer_arbiter_heartbeat) {
        send_arbiter_heartbeat();
        return;
    }
}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void MatchingEngineThread::send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, int32_t session_id, std::string_view cl_ord_id,
                                            const OrderEntry* entry) {
    if (!ha_enabled_ || !is_primary_ || !secondary_replication_conn_id_.is_valid()) {
        return;
    }

    pubsub_itc_fw_app::BookUpdate upd{};
    upd.seq_no = seq_no;
    upd.update_type = static_cast<int8_t>(update_type);
    upd.session_id = session_id;
    upd.cl_ord_id = cl_ord_id;

    if (entry != nullptr) {
        upd.order_id_num = entry->order_id_num;
        upd.side = static_cast<int8_t>(entry->side);
        upd.ord_type = static_cast<int8_t>(entry->ord_type);
        upd.symbol = entry->get_symbol();
        upd.order_qty = entry->get_order_qty();
        if (entry->has_price) {
            upd.has_price = true;
            upd.price = entry->get_price();
        }
    }

    send_pdu(secondary_replication_conn_id_, pdu_book_update, seq_no, upd);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: BookUpdate sent seq={} type={} session={} cl_ord_id={}", seq_no,
               static_cast<int>(update_type), session_id, cl_ord_id);
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

    const OrderKey key = OrderKey::make(view.session_id, view.cl_ord_id);
    const auto update_type = static_cast<pubsub_itc_fw_app::BookUpdateType>(view.update_type);

    if (update_type == pubsub_itc_fw_app::BookUpdateType::Add) {
        OrderEntry entry{};
        entry.order_id_num = view.order_id_num;
        entry.gateway_session_conn_id = view.session_id;
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

void MatchingEngineThread::adopt_leader_role() {
    if (ha_role_state_ == MeRole::Leader) {
        return;
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: adopting LEADER role (epoch={})", epoch_);
    ha_role_state_ = MeRole::Leader;
    write_fence_file();
    // Renew the arbiter lease periodically for as long as we are leader.
    cancel_timer(timer_arbiter_heartbeat);
    start_recurring_timer(timer_arbiter_heartbeat, std::chrono::seconds(config_.heartbeat_interval_seconds));
    // Send one heartbeat immediately so the arbiter registers us without delay.
    send_arbiter_heartbeat();
}

void MatchingEngineThread::send_arbitration_report() {
    pubsub_itc_fw_app::ArbitrationReport report{};
    report.self_instance_id = static_cast<int64_t>(config_.instance_id);
    report.peer_instance_id = primary_instance_id;
    report.epoch = epoch_;
    report.proposed_role = pubsub_itc_fw_app::Role::leader;
    report.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_arbitration_report, 0, report);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_arbitration_report, 0, report);
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
    if (ha_role_state_ == MeRole::Reconciling || ha_role_state_ == MeRole::Leader) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: already {} -- ignoring duplicate ArbitrationDecision",
                   ha_role_state_ == MeRole::Leader ? "LEADER" : "RECONCILING");
        return;
    }

    epoch_ = decision.epoch;

    if (decision.leader_instance_id == static_cast<int64_t>(config_.instance_id)) {
        // We are the leader: begin WAL reconciliation, then adopt LEADER.
        begin_reconciliation();
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
    ha_role_state_ = MeRole::Follower;
    cancel_timer(timer_arbiter_heartbeat);
}

void MatchingEngineThread::send_arbiter_heartbeat() {
    pubsub_itc_fw_app::Heartbeat hb{};
    hb.instance_id = static_cast<int64_t>(config_.instance_id);
    hb.epoch = epoch_;
    hb.group = pubsub_itc_fw_app::ComponentGroup::matching_engine;
    if (arbiter_primary_conn_id_.is_valid()) {
        send_pdu(arbiter_primary_conn_id_, pdu_heartbeat, 0, hb);
    }
    if (arbiter_secondary_conn_id_.is_valid()) {
        send_pdu(arbiter_secondary_conn_id_, pdu_heartbeat, 0, hb);
    }
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: arbiter heartbeat sent (instance_id={} epoch={})", hb.instance_id,
               hb.epoch);
}

void MatchingEngineThread::write_fence_file() const {
    const std::string& path = config_.fence_file_path;
    const std::string content = std::to_string(config_.instance_id) + "\n";
    FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: failed to write fence file '{}': {}", path, std::strerror(errno));
        return;
    }
    std::fputs(content.c_str(), f);
    std::fclose(f);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: fence file written: {}", path);
}

// WAL reconciliation (Slice D)

void MatchingEngineThread::begin_reconciliation() {
    // Cancel the promotion timer (it fired or arbitration is complete).
    cancel_timer(timer_promotion_timeout);
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
        send_pdu(conn_id, pdu_me_position_request, 0, request);
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
        er.has_gateway_session_conn_id = true;
        er.gateway_session_conn_id = entry.gateway_session_conn_id;

        // seq_no 0: these cancels are generated by the ME on promotion, not driven
        // by a sequenced order. The sequencer routes by gateway_session_conn_id.
        send_er_to_sequencer(er, 0);
        ++cancelled;
    }

    order_book_.clear();
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: cancel-on-failover complete -- {} cancel ER(s) sent, book cleared",
               cancelled);
}

} // namespaces
