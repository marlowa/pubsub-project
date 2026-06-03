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

} // namespace

MatchingEngineThread::MatchingEngineThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                           pubsub_itc_fw::Reactor& reactor, const MatchingEngineConfiguration& config)
    : ApplicationThread(token, logger, reactor, "MatchingEngineThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , sequencer_er_conn_id_{}
    , sequencer_er_secondary_conn_id_{} {
    order_book_.reserve(static_cast<size_t>(config_.order_book_initial_capacity));
}

void MatchingEngineThread::on_app_ready_event() {
    connect_to_service("sequencer_er");
    connect_to_service("sequencer_er_secondary");
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: primary sequencer ER connection {} established", id.get_value());
    } else if (id.service_name() == "sequencer_er_secondary") {
        sequencer_er_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: secondary sequencer ER connection {} established", id.get_value());
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequencer order connection {} established", id.get_value());
    }
}

void MatchingEngineThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: primary sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else if (id == sequencer_er_secondary_conn_id_) {
        sequencer_er_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: secondary sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequencer order connection {} lost: {}", id.get_value(), reason);
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

    const std::string cl_ord_id  = std::string(view.cl_ord_id);
    const std::string symbol     = std::string(view.symbol);
    const std::string order_qty  = std::string(view.order_qty);
    const std::string price      = view.has_price ? std::string(view.price) : std::string{};
    const std::string exec_id    = generate_exec_id();
    const int64_t now_ns = view.has_sequenced_at
                               ? view.sequenced_at
                               : config_.wall_clock->now_ns();

    // Reject duplicate ClOrdID.
    if (order_book_.count(cl_ord_id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: duplicate ClOrdID={} -- rejecting NOS", cl_ord_id);

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id     = "NONE";
        er.exec_id      = exec_id;
        er.exec_type    = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status   = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol       = symbol;
        er.side         = view.side;
        er.leaves_qty   = "0";
        er.cum_qty      = "0";
        er.avg_px       = "0.00";
        er.transact_time = now_ns;
        er.has_cl_ord_id      = true;
        er.cl_ord_id          = cl_ord_id;
        er.has_order_qty      = true;
        er.order_qty          = order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason     = pubsub_itc_fw_app::OrdRejReason::DuplicateOrder;

        send_er_to_sequencer(er, sequence_number);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngineThread: sent rejection ER ExecID={} ClOrdID={} (DuplicateOrder)", exec_id, cl_ord_id);
        return;
    }

    // Accept the order: add to book and acknowledge with ExecType=New.
    const std::string order_id = generate_order_id();
    OrderEntry entry{};
    entry.order_id  = order_id;
    entry.symbol    = symbol;
    entry.side      = view.side;
    entry.order_qty = order_qty;
    entry.has_price = view.has_price;
    entry.price     = price;
    entry.ord_type  = view.ord_type;
    order_book_.emplace(cl_ord_id, std::move(entry));

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id     = order_id;
    er.exec_id      = exec_id;
    er.exec_type    = pubsub_itc_fw_app::ExecType::New;
    er.ord_status   = pubsub_itc_fw_app::OrdStatus::New;
    er.symbol       = symbol;
    er.side         = view.side;
    er.leaves_qty   = order_qty;
    er.cum_qty      = "0";
    er.avg_px       = "0.00";
    er.transact_time = now_ns;
    er.has_cl_ord_id = true;
    er.cl_ord_id     = cl_ord_id;
    er.has_order_qty = true;
    er.order_qty     = order_qty;
    er.has_ord_type  = true;
    er.ord_type      = view.ord_type;
    if (view.has_price) {
        er.has_price = true;
        er.price     = price;
    }

    send_er_to_sequencer(er, sequence_number);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: accepted NOS OrderID={} ExecID={} ClOrdID={} book_size={}",
               order_id, exec_id, cl_ord_id, order_book_.size());
}

void MatchingEngineThread::handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t sequence_number) {
    if (!sequencer_er_conn_id_.is_valid() && !sequencer_er_secondary_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: no sequencer ER connections established -- dropping OCR");
        return;
    }

    const std::string cl_ord_id      = std::string(view.cl_ord_id);
    const std::string orig_cl_ord_id = std::string(view.orig_cl_ord_id);
    const std::string order_qty      = std::string(view.order_qty);
    const std::string exec_id        = generate_exec_id();
    const int64_t now_ns = view.has_sequenced_at
                               ? view.sequenced_at
                               : config_.wall_clock->now_ns();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: OrderCancelRequest seq={} ClOrdID={} OrigClOrdID={} Symbol={} Side={}",
               sequence_number, cl_ord_id, orig_cl_ord_id, view.symbol, static_cast<char>(view.side));

    auto it = order_book_.find(orig_cl_ord_id);
    if (it == order_book_.end()) {
        // OrigClOrdID not in the book — order is unknown.
        const std::string symbol = std::string(view.symbol);

        pubsub_itc_fw_app::ExecutionReport er{};
        er.order_id      = "NONE";
        er.exec_id       = exec_id;
        er.exec_type     = pubsub_itc_fw_app::ExecType::Rejected;
        er.ord_status    = pubsub_itc_fw_app::OrdStatus::Rejected;
        er.symbol        = symbol;
        er.side          = view.side;
        er.leaves_qty    = "0";
        er.cum_qty       = "0";
        er.avg_px        = "0.00";
        er.transact_time = now_ns;
        er.has_cl_ord_id      = true;
        er.cl_ord_id          = cl_ord_id;
        er.has_orig_cl_ord_id = true;
        er.orig_cl_ord_id     = orig_cl_ord_id;
        er.has_order_qty      = true;
        er.order_qty          = order_qty;
        er.has_ord_rej_reason = true;
        er.ord_rej_reason     = pubsub_itc_fw_app::OrdRejReason::UnknownOrder;

        send_er_to_sequencer(er, sequence_number);
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "MatchingEngineThread: sent rejection ER ExecID={} OrigClOrdID={} (UnknownOrder)", exec_id, orig_cl_ord_id);
        return;
    }

    // Order found — cancel it.
    const OrderEntry entry = std::move(it->second);
    order_book_.erase(it);

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id      = entry.order_id;
    er.exec_id       = exec_id;
    er.exec_type     = pubsub_itc_fw_app::ExecType::Canceled;
    er.ord_status    = pubsub_itc_fw_app::OrdStatus::Canceled;
    er.symbol        = entry.symbol;
    er.side          = entry.side;
    er.leaves_qty    = "0";
    er.cum_qty       = "0";
    er.avg_px        = "0.00";
    er.transact_time = now_ns;
    er.has_cl_ord_id      = true;
    er.cl_ord_id          = cl_ord_id;
    er.has_orig_cl_ord_id = true;
    er.orig_cl_ord_id     = orig_cl_ord_id;
    er.has_order_qty      = true;
    er.order_qty          = entry.order_qty;
    if (entry.has_price) {
        er.has_price = true;
        er.price     = entry.price;
    }
    er.has_ord_type = true;
    er.ord_type     = entry.ord_type;

    send_er_to_sequencer(er, sequence_number);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngineThread: sent cancel ER OrderID={} ExecID={} ClOrdID={} OrigClOrdID={} book_size={}",
               entry.order_id, exec_id, cl_ord_id, orig_cl_ord_id, order_book_.size());
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

std::string MatchingEngineThread::generate_order_id() {
    return "ME-ORD-" + std::to_string(++order_id_counter_);
}

std::string MatchingEngineThread::generate_exec_id() {
    return "ME-EXEC-" + std::to_string(++exec_id_counter_);
}

void MatchingEngineThread::on_timer_event([[maybe_unused]] const std::string& name) {}

void MatchingEngineThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

} // namespace matching_engine
