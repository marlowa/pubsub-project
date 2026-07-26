// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "BinaryGatewayThread.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace binary_gateway {

namespace {

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const BinaryGatewayConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "BinaryGatewayPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

// Sent per drain tick. A client with thousands of resting orders would otherwise stall
// the reactor while every cancel was encoded and forwarded in one go.
constexpr int cancel_drain_batch_size = 500;
constexpr auto cancel_drain_interval = std::chrono::milliseconds{1};

/** @brief True for an ExecutionReport status that means the order has left the book. */
bool is_terminal_ord_status(pubsub_itc_fw_app::OrdStatus status) {
    switch (status) {
        case pubsub_itc_fw_app::OrdStatus::Filled:
        case pubsub_itc_fw_app::OrdStatus::Canceled:
        case pubsub_itc_fw_app::OrdStatus::DoneForDay:
        case pubsub_itc_fw_app::OrdStatus::Rejected:
        case pubsub_itc_fw_app::OrdStatus::Expired:
            return true;
        default:
            return false;
    }
}

} // namespaces

BinaryGatewayThread::BinaryGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                         pubsub_itc_fw::Reactor& reactor, const BinaryGatewayConfiguration& config)
    : ApplicationThread(token, logger, reactor, "BinaryGatewayThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , sequencer_primary_conn_id_{}
    , sequencer_secondary_conn_id_{}
    , client_inbound_service_("inbound:" + std::to_string(config.listen_port))
    , er_inbound_service_("inbound:" + std::to_string(config.er_listen_port)) {}

void BinaryGatewayThread::on_app_ready_event() {
    open_order_pool_ = std::make_unique<pubsub_itc_fw::ExpandablePoolAllocator<open_orders::OpenOrderEntry>>(
        "BinaryOpenOrderPool", config_.open_order_pool_objects_per_pool, config_.open_order_pool_initial_pools,
        /*expansion_threshold_hint=*/0,
        /*handler_for_pool_exhausted=*/
        [this](void* /*context*/, int objects_per_pool) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryOpenOrderPool exhausted: chaining new pool slab ({} objects)",
                       objects_per_pool);
        },
        /*handler_for_invalid_free=*/nullptr,
        /*handler_for_huge_pages_error=*/nullptr, pubsub_itc_fw::UseHugePagesFlag{pubsub_itc_fw::UseHugePagesFlag::DoNotUseHugePages});

    connect_to_service("sequencer_primary");
    if (config_.ha_enabled) {
        connect_to_service("sequencer_secondary");
    }
}

void BinaryGatewayThread::on_timer_event(pubsub_itc_fw::TimerID timer_id) {
    if (timer_id == cancel_drain_timer_id_) {
        drain_pending_cancels();
    }
}

void BinaryGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    const std::string& service = id.service_name();

    if (service == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: primary sequencer connection {} established", id.get_value());
        return;
    }
    if (service == "sequencer_secondary") {
        sequencer_secondary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: secondary sequencer connection {} established", id.get_value());
        return;
    }
    if (service == er_inbound_service_) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: sequencer ER connection {} established", id.get_value());
        return;
    }
    if (service == client_inbound_service_) {
        BinarySession session{};
        session.conn_id = id;
        sessions_.emplace(id.get_value(), std::move(session));
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: client connection {} accepted, awaiting Logon", id.get_value());
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: connection {} established on unexpected service '{}'", id.get_value(),
               service);
}

void BinaryGatewayThread::on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) {
    if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: primary sequencer connection {} lost: {}", id.get_value(), reason);
        return;
    }
    if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: secondary sequencer connection {} lost: {}", id.get_value(), reason);
        return;
    }

    auto it = sessions_.find(id.get_value());
    if (it != sessions_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: client connection {} (comp id '{}') lost: {}", id.get_value(),
                   it->second.comp_id, reason);
        // The client is gone but its orders are still on the book with nobody managing
        // them, so cancel them on its behalf before the session is destroyed.
        queue_session_for_cleanup(it->second);
        sessions_.erase(it);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "BinaryGatewayThread: connection {} lost: {}", id.get_value(), reason);
}

void BinaryGatewayThread::on_itc_message(const pubsub_itc_fw::EventMessage& /*message*/) {}

void BinaryGatewayThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) {
    // ERs arrive on the connection the sequencer dialled; everything else on this thread
    // is a client. Telling them apart by service name rather than by PDU id means a
    // client cannot reach the ER path by sending a PDU it has no business sending.
    if (message.connection_id().service_name() == er_inbound_service_) {
        handle_execution_report(message);
        return;
    }

    BinarySession* session = find_session(message.connection_id());
    if (session == nullptr) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: PDU id {} on unknown connection {} -- dropping", message.pdu_id(),
                   message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    const int16_t pdu_id = message.pdu_id();

    if (pdu_id == pubsub_itc_fw_app::Logon::message_pdu_id) {
        handle_logon(*session, message);
        return;
    }

    if (!session->logged_on) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: PDU id {} on connection {} before Logon -- disconnecting", pdu_id,
                   message.connection_id().get_value());
        release_pdu_payload(message);
        pubsub_itc_fw::ReactorControlCommand command(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        command.connection_id_ = message.connection_id();
        get_reactor().enqueue_control_command(command);
        return;
    }

    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle)) {
        handle_new_order_single(*session, message);
        return;
    }
    if (pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        handle_order_cancel_request(*session, message);
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: unsupported PDU id {} from comp id '{}' on connection {} -- dropping",
               pdu_id, session->comp_id, message.connection_id().get_value());
    release_pdu_payload(message);
}

void BinaryGatewayThread::handle_logon(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    pubsub_itc_fw_app::LogonView view{};
    const bool decoded =
        pubsub_itc_fw_app::decode(view, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed);
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();
    release_pdu_payload(message);

    if (!decoded) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: failed to decode Logon on connection {} -- disconnecting",
                   conn_id.get_value());
        send_logon_ack(conn_id, pubsub_itc_fw_app::LogonOutcome::MissingCompId, "Logon could not be decoded");
        return;
    }

    if (session.logged_on) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: second Logon on connection {} (comp id '{}') -- disconnecting",
                   conn_id.get_value(), session.comp_id);
        send_logon_ack(conn_id, pubsub_itc_fw_app::LogonOutcome::AlreadyLoggedOn, "session is already logged on");
        return;
    }

    if (view.comp_id.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: Logon with empty comp id on connection {} -- disconnecting",
                   conn_id.get_value());
        send_logon_ack(conn_id, pubsub_itc_fw_app::LogonOutcome::MissingCompId, "comp_id must not be empty");
        return;
    }

    if (comp_id_in_use(view.comp_id)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryGatewayThread: Logon for comp id '{}' already logged on -- disconnecting connection {}", view.comp_id, conn_id.get_value());
        send_logon_ack(conn_id, pubsub_itc_fw_app::LogonOutcome::DuplicateCompId, "comp_id is already logged on");
        return;
    }

    session.comp_id.assign(view.comp_id.data(), view.comp_id.size());
    session.logged_on = true;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: connection {} logged on as '{}'", conn_id.get_value(), session.comp_id);
    send_logon_ack(conn_id, pubsub_itc_fw_app::LogonOutcome::Accepted, {});
}

void BinaryGatewayThread::send_logon_ack(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::LogonOutcome outcome, std::string_view text) {
    pubsub_itc_fw_app::LogonAck ack{};
    ack.outcome = outcome;
    if (!text.empty()) {
        ack.has_text = true;
        ack.text = text;
    }
    send_pdu(conn_id, pubsub_itc_fw_app::LogonAck::message_pdu_id, 0, ack);

    if (outcome != pubsub_itc_fw_app::LogonOutcome::Accepted) {
        // The ack is queued before the disconnect command, so the client gets its reason
        // rather than an unexplained close.
        pubsub_itc_fw::ReactorControlCommand command(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
        command.connection_id_ = conn_id;
        get_reactor().enqueue_control_command(command);
    }
}

void BinaryGatewayThread::handle_new_order_single(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle), message.payload(),
                              static_cast<size_t>(message.payload_size()), session);
    release_pdu_payload(message);
}

void BinaryGatewayThread::handle_order_cancel_request(BinarySession& session, const pubsub_itc_fw::EventMessage& message) {
    forward_order_in_envelope(static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest), message.payload(),
                              static_cast<size_t>(message.payload_size()), session);
    release_pdu_payload(message);
}

void BinaryGatewayThread::forward_order_in_envelope(int16_t inner_pdu_id, const uint8_t* payload, size_t size, const BinarySession& session) {
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.pdu_id = inner_pdu_id;
    envelope.payload.data = payload;
    envelope.payload.size = size;
    // The sequencer stamps seq_no and wall_time_ns; both are left zero here.
    envelope.has_gateway_session_conn_id = true;
    envelope.gateway_session_conn_id = session.conn_id.get_value();
    envelope.has_origin_gateway_id = true;
    envelope.origin_gateway_id = gateway_ids::binary_gateway;
    if (!session.comp_id.empty()) {
        envelope.has_sender_comp_id = true;
        envelope.sender_comp_id = session.comp_id;
    }

    forward_envelope_to_sequencers(envelope);
}

void BinaryGatewayThread::forward_envelope_to_sequencers(const pubsub_itc_fw_app::WalRecord& envelope) {
    if (sequencer_primary_conn_id_.get_value() != 0) {
        send_pdu(sequencer_primary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, 0, envelope);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "BinaryGatewayThread: primary sequencer not connected -- order not forwarded to primary");
    }
    if (config_.ha_enabled) {
        if (sequencer_secondary_conn_id_.get_value() != 0) {
            send_pdu(sequencer_secondary_conn_id_, pubsub_itc_fw_app::WalRecord::message_pdu_id, 0, envelope);
        } else {
            PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "BinaryGatewayThread: secondary sequencer not connected -- order not forwarded to secondary");
        }
    }
}

void BinaryGatewayThread::handle_execution_report(const pubsub_itc_fw::EventMessage& message) {
    if (message.pdu_id() != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: unexpected PDU id {} from the sequencer -- dropping",
                   message.pdu_id());
        release_pdu_payload(message);
        return;
    }

    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    // Only the envelope is decoded. The ER inside it is relayed to the client as the
    // bytes that arrived -- this gateway has no reason to understand a message it
    // merely delivers, and not decoding it means it needs no change when the ER gains
    // a field.
    pubsub_itc_fw_app::WalRecordView envelope{};
    if (!pubsub_itc_fw_app::decode(envelope, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: failed to decode the ER envelope -- dropping");
        release_pdu_payload(message);
        return;
    }

    if (!envelope.has_gateway_session_conn_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayThread: ER seq={} carries no session connection id -- dropping",
                   envelope.seq_no);
        release_pdu_payload(message);
        return;
    }

    auto it = sessions_.find(envelope.gateway_session_conn_id);
    if (it == sessions_.end()) {
        // The ordinary case is a client that disconnected while its order was in flight,
        // including the acknowledgements of the cancels sent on its behalf.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: ER seq={} for connection {} which is no longer present -- dropping",
                   envelope.seq_no, envelope.gateway_session_conn_id);
        release_pdu_payload(message);
        return;
    }

    // Decode the report to maintain the open-order set. The bytes relayed to the client
    // are still the ones that arrived -- this reads the report, it does not rebuild it.
    pubsub_itc_fw_app::ExecutionReportView report{};
    size_t report_bytes_consumed = 0;
    if (pubsub_itc_fw_app::decode(report, envelope.payload.data, envelope.payload.size, report_bytes_consumed, arena, arena_bytes_needed)) {
        track_open_order(it->second, report);
    } else {
        // Relay it regardless: the client is the report's audience, and a gateway that
        // cannot read a message still has no business withholding it.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "BinaryGatewayThread: ER seq={} could not be decoded for order tracking -- relaying it but not tracking the order", envelope.seq_no);
    }

    send_pdu_payload(it->second.conn_id, envelope.pdu_id, envelope.seq_no, envelope.payload.data, envelope.payload.size);
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "BinaryGatewayThread: ER seq={} relayed to connection {} ('{}')", envelope.seq_no,
               envelope.gateway_session_conn_id, it->second.comp_id);
    release_pdu_payload(message);
}

void BinaryGatewayThread::track_open_order(BinarySession& session, const pubsub_itc_fw_app::ExecutionReportView& report) {
    if (!report.has_cl_ord_id) {
        return;
    }

    if (is_terminal_ord_status(report.ord_status)) {
        // Looking up by string_view compares contents, so this finds the entry keyed by
        // the pool storage without building a std::string.
        auto existing = session.open_orders.find(std::string_view(report.cl_ord_id));
        if (existing != session.open_orders.end()) {
            open_order_pool_->deallocate(existing->second);
            session.open_orders.erase(existing);
        }
        return;
    }

    // Over-long values would overrun the entry's fixed arrays. The gateway validates
    // ClOrdID at ingress, and symbol and quantity come from the matching engine rather
    // than the client, so this is a guard against a pipeline defect, not client input.
    const size_t cl_ord_id_length = report.cl_ord_id.size();
    const size_t symbol_length = report.symbol.size();
    const size_t order_qty_length = report.has_order_qty ? report.order_qty.size() : 0;
    if (cl_ord_id_length > fix_order_limits::max_cl_ord_id_length || symbol_length > open_orders::max_supported_symbol_length ||
        order_qty_length > open_orders::max_supported_order_qty_length) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "BinaryGatewayThread: ExecutionReport field too long to track (ClOrdID {} bytes, symbol {}, qty {}) -- order not tracked for cancel",
                   cl_ord_id_length, symbol_length, order_qty_length);
        return;
    }

    // A repeated non-terminal report for an order already tracked -- a partial fill, say --
    // updates the entry in place. Allocating a second one would strand the first in the
    // pool, since the map can only hold one entry per ClOrdID.
    auto tracked = session.open_orders.find(std::string_view(report.cl_ord_id));
    const bool already_tracked = tracked != session.open_orders.end();
    open_orders::OpenOrderEntry* entry = already_tracked ? tracked->second : open_order_pool_->allocate();

    std::memcpy(entry->cl_ord_id, report.cl_ord_id.data(), cl_ord_id_length);
    entry->cl_ord_id[cl_ord_id_length] = '\0';
    entry->cl_ord_id_len = static_cast<uint8_t>(cl_ord_id_length);
    std::memcpy(entry->symbol, report.symbol.data(), symbol_length);
    entry->symbol[symbol_length] = '\0';
    entry->symbol_len = static_cast<uint8_t>(symbol_length);
    if (order_qty_length > 0) {
        std::memcpy(entry->order_qty, report.order_qty.data(), order_qty_length);
    }
    entry->order_qty[order_qty_length] = '\0';
    entry->order_qty_len = static_cast<uint8_t>(order_qty_length);
    entry->side = static_cast<char>(report.side);

    if (!already_tracked) {
        // The key views the pool storage, which is stable for the entry's lifetime.
        session.open_orders.emplace(std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), entry);
    }
}

void BinaryGatewayThread::queue_session_for_cleanup(BinarySession& session) {
    if (session.open_orders.empty()) {
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: connection {} disconnected with {} open order(s) -- queuing cancels",
               session.conn_id.get_value(), session.open_orders.size());

    DeadSession dead;
    // Collected into a flat vector rather than moving the map: the orders are only ever
    // consumed in sequence from here, and draining a map by repeatedly taking begin()
    // rescans the buckets already emptied, which is quadratic in the order count.
    dead.open_orders.reserve(session.open_orders.size());
    for (const auto& [cl_ord_id, entry] : session.open_orders) {
        dead.open_orders.push_back(entry);
    }
    session.open_orders.clear();
    dead.session_conn_id = session.conn_id.get_value();
    dead.comp_id = session.comp_id;
    dead.cancel_id_counter = session.cancel_id_counter;
    pending_cancel_sessions_.push_back(std::move(dead));

    if (!cancel_drain_timer_active_) {
        cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
        cancel_drain_timer_active_ = true;
    }
}

void BinaryGatewayThread::drain_pending_cancels() {
    cancel_drain_timer_active_ = false;

    int sent = 0;
    while (!pending_cancel_sessions_.empty() && sent < cancel_drain_batch_size) {
        DeadSession& dead = pending_cancel_sessions_.front();

        if (dead.next_order_index >= dead.open_orders.size()) {
            pending_cancel_sessions_.pop_front();
            continue;
        }

        open_orders::OpenOrderEntry* entry = dead.open_orders[dead.next_order_index];

        const std::string cancel_cl_ord_id = "BGW-CXL-" + std::to_string(dead.session_conn_id) + "-" + std::to_string(dead.cancel_id_counter++);

        pubsub_itc_fw_app::OrderCancelRequest cancel{};
        cancel.orig_cl_ord_id = std::string_view(entry->cl_ord_id, entry->cl_ord_id_len);
        cancel.cl_ord_id = cancel_cl_ord_id;
        cancel.symbol = std::string_view(entry->symbol, entry->symbol_len);
        cancel.side = static_cast<pubsub_itc_fw_app::Side>(entry->side);
        cancel.transact_time = 0; // the sequencer stamps the authoritative time
        if (entry->order_qty_len > 0) {
            cancel.order_qty = std::string_view(entry->order_qty, entry->order_qty_len);
        }

        // Unlike a client order, this one is built here rather than relayed, so it has to
        // be encoded before it can be wrapped. Measure then fit, into a reused buffer.
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        static_cast<void>(encode(cancel, nullptr, 0, bytes_written, bytes_needed));
        if (cancel_encode_buffer_.size() < bytes_needed) {
            cancel_encode_buffer_.resize(bytes_needed);
        }
        if (!encode(cancel, cancel_encode_buffer_.data(), cancel_encode_buffer_.size(), bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                       "BinaryGatewayThread: failed to encode the cancel for ClOrdID '{}' ({} bytes needed) -- order left on the book",
                       std::string_view(entry->cl_ord_id, entry->cl_ord_id_len), bytes_needed);
        } else {
            pubsub_itc_fw_app::WalRecord envelope{};
            envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest);
            envelope.payload.data = cancel_encode_buffer_.data();
            envelope.payload.size = bytes_written;
            // The departed session's identity still rides on the envelope so the cancel
            // is attributed to the client whose order it retires.
            envelope.has_gateway_session_conn_id = true;
            envelope.gateway_session_conn_id = dead.session_conn_id;
            envelope.has_origin_gateway_id = true;
            envelope.origin_gateway_id = gateway_ids::binary_gateway;
            if (!dead.comp_id.empty()) {
                envelope.has_sender_comp_id = true;
                envelope.sender_comp_id = dead.comp_id;
            }
            forward_envelope_to_sequencers(envelope);
        }

        open_order_pool_->deallocate(entry);
        ++dead.next_order_index;
        ++sent;
    }

    if (!pending_cancel_sessions_.empty()) {
        cancel_drain_timer_id_ = start_one_off_timer(cancel_drain_interval);
        cancel_drain_timer_active_ = true;
    } else if (sent > 0) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "BinaryGatewayThread: cancel drain complete");
    }
}

BinarySession* BinaryGatewayThread::find_session(const pubsub_itc_fw::ConnectionID& id) {
    auto it = sessions_.find(id.get_value());
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

bool BinaryGatewayThread::comp_id_in_use(std::string_view comp_id) const {
    for (const auto& [conn_id, session] : sessions_) {
        if (session.logged_on && session.comp_id == comp_id) {
            return true;
        }
    }
    return false;
}

} // namespaces
