// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngineThread.hpp"

// <chrono> not required here; time is obtained via the injected WallClock.

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
}

void MatchingEngineThread::on_app_ready_event() {
    if (is_primary_) {
        connect_to_service("sequencer_er");
        connect_to_service("sequencer_er_secondary");
        if (ha_enabled_) {
            connect_to_service("me_secondary_replication");
        }
    }
    // Secondary: no outbound connections in Slice A+B; it only receives inbound book updates.
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: primary sequencer ER connection {} established", id.get_value());
    } else if (id.service_name() == "sequencer_er_secondary") {
        sequencer_er_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
    } else if (id.service_name() == "me_secondary_replication") {
        // Primary: outbound connection to ME-secondary's replication listener established.
        secondary_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: ME-secondary replication connection {} established", id.get_value());
    } else if (!is_primary_ && ha_enabled_) {
        // Secondary: inbound connection from ME-primary (book replication channel).
        primary_replication_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: ME-primary replication connection {} established -- replica book will be updated",
                   id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: inbound sequencer order connection {} established", id.get_value());
    }
}

void MatchingEngineThread::on_connection_lost(const pubsub_itc_fw::ConnectionID &id, const std::string& reason) {
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: primary sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_er_secondary_conn_id_) {
        sequencer_er_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: secondary sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else if (id == secondary_replication_conn_id_) {
        secondary_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-secondary replication connection {} lost: {} -- book updates paused until secondary reconnects",
                   id.get_value(), reason);
    } else if (id == primary_replication_conn_id_) {
        primary_replication_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: ME-primary replication connection {} lost: {} -- replica book is now stale (last seq={})",
                   id.get_value(), reason, last_replicated_seq_no_);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: inbound sequencer order connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEngineThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = message.pdu_id();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "MatchingEngineThread: sequenced PDU received on connection {} pdu_id={}",
               message.connection_id().get_value(), pdu_id);

    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle)) {
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

    } else if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest)) {
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
        return;  // apply_book_update calls release_pdu_payload internally.
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported sequenced PDU id {} -- dropping", pdu_id);
    }

    release_pdu_payload(message);
}

void MatchingEngineThread::handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number) {
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
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Add,
                     session_id, view.cl_ord_id, &entry);

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
    send_book_update(sequence_number, pubsub_itc_fw_app::BookUpdateType::Remove,
                     session_id, view.orig_cl_ord_id, nullptr);

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
    constexpr auto er_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport);
    if (sequencer_er_conn_id_.is_valid()) {
        send_pdu(sequencer_er_conn_id_, er_pdu_id, seq_no, er);
    }
    if (sequencer_er_secondary_conn_id_.is_valid()) {
        send_pdu(sequencer_er_secondary_conn_id_, er_pdu_id, seq_no, er);
    }
}

void MatchingEngineThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void MatchingEngineThread::send_book_update(int64_t seq_no,
                                             pubsub_itc_fw_app::BookUpdateType update_type,
                                             int32_t session_id,
                                             std::string_view cl_ord_id,
                                             const OrderEntry* entry) {
    if (!ha_enabled_ || !is_primary_ || !secondary_replication_conn_id_.is_valid()) {
        return;
    }

    pubsub_itc_fw_app::BookUpdate upd{};
    upd.seq_no      = seq_no;
    upd.update_type = static_cast<int8_t>(update_type);
    upd.session_id  = session_id;
    upd.cl_ord_id   = cl_ord_id;

    if (entry != nullptr) {
        upd.order_id_num = entry->order_id_num;
        upd.side         = static_cast<int8_t>(entry->side);
        upd.ord_type     = static_cast<int8_t>(entry->ord_type);
        upd.symbol       = entry->get_symbol();
        upd.order_qty    = entry->get_order_qty();
        if (entry->has_price) {
            upd.has_price = true;
            upd.price     = entry->get_price();
        }
    }

    send_pdu(secondary_replication_conn_id_, pdu_book_update, seq_no, upd);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "MatchingEngineThread: BookUpdate sent seq={} type={} session={} cl_ord_id={}",
               seq_no, static_cast<int>(update_type), session_id, cl_ord_id);
}

void MatchingEngineThread::apply_book_update(const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::BookUpdateView view{};

    if (!pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()),
                                    bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "MatchingEngineThread: failed to decode BookUpdate -- dropping");
        release_pdu_payload(message);
        return;
    }

    const OrderKey key = OrderKey::make(view.session_id, view.cl_ord_id);
    const auto update_type = static_cast<pubsub_itc_fw_app::BookUpdateType>(view.update_type);

    if (update_type == pubsub_itc_fw_app::BookUpdateType::Add) {
        OrderEntry entry{};
        entry.order_id_num = view.order_id_num;
        entry.side         = static_cast<pubsub_itc_fw_app::Side>(view.side);
        entry.ord_type     = static_cast<pubsub_itc_fw_app::OrdType>(view.ord_type);
        entry.set_symbol(view.symbol);
        entry.set_order_qty(view.order_qty);
        entry.has_price = view.has_price;
        if (view.has_price) {
            entry.set_price(view.price);
        }
        order_book_.insert_or_assign(key, entry);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: replica Add seq={} cl_ord_id={} book_size={}",
                   view.seq_no, view.cl_ord_id, order_book_.size());
    } else if (update_type == pubsub_itc_fw_app::BookUpdateType::Remove) {
        order_book_.erase(key);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
                   "MatchingEngineThread: replica Remove seq={} cl_ord_id={} book_size={}",
                   view.seq_no, view.cl_ord_id, order_book_.size());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: unknown BookUpdate type {} -- dropping", view.update_type);
    }

    const int64_t prev_seq = last_replicated_seq_no_;
    last_replicated_seq_no_ = view.seq_no;

    // Log the first update and every 10,000 seq_nos so progress is visible
    // without flooding the log at Info level.
    if (prev_seq == 0 || view.seq_no % 10000 == 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: replica book seq={} size={}", view.seq_no, order_book_.size());
    }

    release_pdu_payload(message);
}

} // namespaces
