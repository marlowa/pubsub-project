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
    , sequencer_er_conn_id_{} {}

void MatchingEngineThread::on_app_ready_event() {
    connect_to_service("sequencer_er");
}

void MatchingEngineThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    if (id.service_name() == "sequencer_er") {
        sequencer_er_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequencer ER connection {} established", id.get_value());
    } else {
        // Inbound connection from sequencer carrying sequenced order PDUs.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequencer order connection {} established", id.get_value());
    }
}

void MatchingEngineThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (id == sequencer_er_conn_id_) {
        sequencer_er_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: sequencer ER connection {} lost: {}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequencer order connection {} lost: {}", id.get_value(), reason);
    }
}

void MatchingEngineThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    const auto pdu_id = static_cast<int16_t>(message.pdu_id());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sequenced PDU received on connection {} pdu_id={}",
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
        // Pass the sequence number from the incoming message
        handle_new_order_single(view, message.seq_no());

    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: unsupported sequenced PDU id {} -- dropping", pdu_id);
    }

    release_pdu_payload(message);
}

void MatchingEngineThread::handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t sequence_number) {
    if (!sequencer_er_conn_id_.is_valid()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineThread: sequencer ER connection not established -- dropping ER");
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: NewOrderSingle ClOrdID={} Symbol={} Side={} OrderQty={}", view.cl_ord_id,
               view.symbol, static_cast<char>(view.side), view.order_qty);

    /*
     * Fabricate a filled ExecutionReport. There is no real order book here --
     * the matching engine is a stand-in to exercise the round-trip comms path.
     * Every order is reported as fully filled at the requested limit price
     * (or at a hardcoded sentinel price for market orders).
     *
     * The following std::string locals own the fabricated text. They must
     * outlive the send_pdu() call below because ExecutionReport's fields are
     * std::string_view and only borrow into these buffers.
     */
    const std::string order_id = generate_order_id();
    const std::string exec_id = generate_exec_id();
    const std::string cl_ord_id = std::string(view.cl_ord_id);
    const std::string symbol = std::string(view.symbol);
    const std::string order_qty = std::string(view.order_qty);
    const std::string price = view.has_price ? std::string(view.price) : std::string("0.00");

    // CumQty == OrderQty (fully filled) and LeavesQty == 0.
    // LastQty is the size of this fill, here equal to OrderQty.
    const std::string& cum_qty = order_qty;
    const std::string leaves_qty = "0";
    const std::string& last_qty = order_qty;
    const std::string& last_px = price;
    const std::string& avg_px = price;

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    pubsub_itc_fw_app::ExecutionReport er{};
    er.order_id = order_id;
    er.exec_id = exec_id;
    er.exec_type = pubsub_itc_fw_app::ExecType::Trade;
    er.ord_status = pubsub_itc_fw_app::OrdStatus::Filled;
    er.symbol = symbol;
    er.side = view.side;
    er.leaves_qty = leaves_qty;
    er.cum_qty = cum_qty;
    er.avg_px = avg_px;
    er.transact_time = now_ns;

    er.has_cl_ord_id = true;
    er.cl_ord_id = cl_ord_id;
    er.has_order_qty = true;
    er.order_qty = order_qty;
    er.has_last_qty = true;
    er.last_qty = last_qty;
    er.has_last_px = true;
    er.last_px = last_px;
    if (view.has_price) {
        er.has_price = true;
        er.price = price;
    }
    er.has_ord_type = true;
    er.ord_type = view.ord_type;

    constexpr auto er_pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport);
    send_pdu(sequencer_er_conn_id_, er_pdu_id, sequence_number, er);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngineThread: sent ExecutionReport OrderID={} ExecID={} ClOrdID={}", order_id, exec_id,
               cl_ord_id);
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
