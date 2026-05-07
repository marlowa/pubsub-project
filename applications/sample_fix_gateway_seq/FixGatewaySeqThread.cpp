// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewaySeqThread.hpp"

#include <cstring>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sample_fix_gateway_seq {

static pubsub_itc_fw::QueueConfiguration make_queue_config()
{
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static pubsub_itc_fw::AllocatorConfiguration make_allocator_config()
{
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name        = "FixGatewaySeqPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

FixGatewaySeqThread::FixGatewaySeqThread(
    pubsub_itc_fw::ApplicationThread::ConstructorToken token,
    pubsub_itc_fw::QuillLogger& logger,
    pubsub_itc_fw::Reactor& reactor,
    const FixGatewaySeqConfiguration& config)
    : ApplicationThread(token, logger, reactor, "FixGatewaySeqThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , serialiser_(config.sender_comp_id, config.default_target_comp_id)
    , sequencer_primary_conn_id_{}
{}

void FixGatewaySeqThread::on_app_ready_event()
{
    connect_to_service("sequencer_primary");
}

void FixGatewaySeqThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    if (id.service_name() == "sequencer_primary") {
        sequencer_primary_conn_id_ = id;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: primary sequencer connection {} ({}) established",
                   id.get_value(), id.service_name());
    } else if (id.service_name() == "inbound:7010") {
        // Inbound FrameworkPdu connection from a sequencer on the ER listener.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: sequencer ER inbound connection {} established",
                   id.get_value());
    } else {
        // Inbound RawBytes connection -- FIX client on port 9879.
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: FIX client connection {} ({}) established -- active sessions: {}",
                   id.get_value(), id.service_name(), sessions_.size() + 1);
    }
}

void FixGatewaySeqThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                              const std::string& reason)
{
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        cancel_timer(it->second.logon_timeout_timer_name());
        sessions_.erase(it);

        // Sweep any cl_ord_id -> session mappings that pointed at this
        // disconnected session. Without this the map would accumulate stale
        // entries indefinitely. Each such entry would trigger a "session no
        // longer present" log line and drop on the next ER for that ClOrdID.
        for (auto map_it = cl_ord_id_to_session_.begin();
             map_it != cl_ord_id_to_session_.end(); ) {
            if (map_it->second == id) {
                map_it = cl_ord_id_to_session_.erase(map_it);
            } else {
                ++map_it;
            }
        }
    }

    if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: primary sequencer connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id.service_name() == "inbound:7010") {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: sequencer ER inbound connection {} lost: {}",
                   id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: FIX client connection {} lost: {} -- active sessions: {}",
                   id.get_value(), reason, sessions_.size());
    }
}

void FixGatewaySeqThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message)
{
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();
    const uint8_t* data    = message.payload();
    const int      available = message.payload_size();

    if (data == nullptr || available <= 0) {
        return;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "FixGatewaySeqThread: {} raw bytes received on connection {} ({})",
               available, conn_id.get_value(), conn_id.service_name());
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, pubsub_itc_fw::StringUtils::hex_dump(data, available));

    // Create a session on first data from this connection if not already present.
    // TODO need to document why we use piecewise_construct here.

    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) {
        sessions_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(conn_id),
            std::forward_as_tuple(conn_id, get_logger(), [this, conn_id](const FixMessage& msg) {
                auto sit = sessions_.find(conn_id);
                if (sit == sessions_.end()) {
                    return;
                }
                FixSession& session = sit->second;
                const std::string& type = msg.msg_type();

                if (type == MsgType::Logon) {
                    handle_logon(session, msg);
                } else if (!session.session_established) {
                    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                               "FixGatewaySeqThread: connection {} ({}) MsgType='{}' before Logon -- disconnecting",
                               conn_id.get_value(), conn_id.service_name(), type);
                    disconnect_session(session, "first message was not Logon");
                } else if (type == MsgType::Heartbeat) {
                    handle_heartbeat(session, msg);
                } else if (type == MsgType::TestRequest) {
                    handle_test_request(session, msg);
                } else if (type == MsgType::Logout) {
                    handle_logout(session, msg);
                } else if (type == MsgType::NewOrderSingle) {
                    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "about to call handle_new_order_single");
                    handle_new_order_single(session, msg);
                } else if (type == MsgType::OrderCancelRequest) {
                    handle_order_cancel_request(session, msg);
                } else {
                    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                               "FixGatewaySeqThread: connection {} ({}) ignoring MsgType='{}'",
                               conn_id.get_value(), conn_id.service_name(), type);
                }
            }));

        start_one_off_timer(sessions_.at(conn_id).logon_timeout_timer_name(),
                            config_.logon_timeout);

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: connection {} new FIX session, waiting for Logon "
                   "(timeout {}s) -- active sessions: {}",
                   conn_id.get_value(), config_.logon_timeout.count(), sessions_.size());

        it = sessions_.find(conn_id);
    }

    FixSession& session = it->second;

    if (!session.preamble_verified) {
        const std::size_t bytes_to_check =
            std::min(static_cast<std::size_t>(available), expected_preamble.size());

        if (std::memcmp(data, expected_preamble.data(), bytes_to_check) != 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                       "FixGatewaySeqThread: connection {} invalid FIX preamble -- disconnecting",
                       conn_id.get_value());
            disconnect_session(session, "invalid FIX preamble");
            commit_raw_bytes(conn_id, static_cast<int64_t>(available));
            return;
        }

        if (static_cast<std::size_t>(available) < expected_preamble.size()) {
            commit_raw_bytes(conn_id, 0);
            return;
        }

        session.preamble_verified = true;
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                   "FixGatewaySeqThread: connection {} FIX preamble verified",
                   conn_id.get_value());
    }

    session.parser.feed(data, available);
    commit_raw_bytes(conn_id, static_cast<int64_t>(available));
}

void FixGatewaySeqThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message)
{
    const int16_t pdu_id = static_cast<int16_t>(message.pdu_id());

    if (pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::ExecutionReport)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
            "FixGatewaySeqThread: unsupported PDU id {} on connection {} -- dropping",
            pdu_id, message.connection_id().get_value());
        release_pdu_payload(message);
        return;
    }

    auto& arena_buf = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buf.data(), arena_buf.size());
    arena.reset();
    std::size_t arena_bytes_needed = 0;
    std::size_t bytes_consumed     = 0;
    pubsub_itc_fw_app::ExecutionReportView view{};

    if (!pubsub_itc_fw_app::decode(view,
                                    message.payload(),
                                    static_cast<std::size_t>(message.payload_size()),
                                    bytes_consumed,
                                    arena,
                                    arena_bytes_needed)) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
            "FixGatewaySeqThread: failed to decode ExecutionReport -- dropping");
        release_pdu_payload(message);
        return;
    }

    // ClOrdID is the only routing key. Without it we cannot deliver the ER.
    if (!view.has_cl_ord_id || view.cl_ord_id.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
            "FixGatewaySeqThread: ExecutionReport OrderID={} ExecID={} has no ClOrdID -- dropping",
            view.order_id, view.exec_id);
        release_pdu_payload(message);
        return;
    }

    // The cl_ord_id_to_session_ map keys are std::string. The view's cl_ord_id
    // is a string_view into the slab; construct a std::string for the lookup
    // which the unordered_map's heterogeneous-lookup-free find() requires.
    const std::string cl_ord_id{view.cl_ord_id};

    auto map_it = cl_ord_id_to_session_.find(cl_ord_id);
    if (map_it == cl_ord_id_to_session_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
            "FixGatewaySeqThread: ExecutionReport for unknown ClOrdID='{}' -- dropping "
            "(originating session may have disconnected)",
            cl_ord_id);
        release_pdu_payload(message);
        return;
    }
    const pubsub_itc_fw::ConnectionID session_id = map_it->second;

    auto session_it = sessions_.find(session_id);
    if (session_it == sessions_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
            "FixGatewaySeqThread: ExecutionReport for ClOrdID='{}' -- "
            "session connection {} no longer present, dropping ER and clearing map entry",
            cl_ord_id, session_id.get_value());
        cl_ord_id_to_session_.erase(map_it);
        release_pdu_payload(message);
        return;
    }
    FixSession& session = session_it->second;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewaySeqThread: routing ExecutionReport OrderID={} ExecID={} ClOrdID={} "
        "to FIX client connection {}",
        view.order_id, view.exec_id, cl_ord_id, session.conn_id.get_value());

    // Build a FIX ExecutionReport. Single-character enum fields convert back
    // to FIX wire chars via static_cast<char>; numeric strings (qty/px) flow
    // through unchanged.
    FixMessage er_msg;
    er_msg.set(Tag::MsgType,   MsgType::ExecutionReport);
    er_msg.set(Tag::OrderID,   std::string{view.order_id});
    er_msg.set(Tag::ExecID,    std::string{view.exec_id});
    er_msg.set(Tag::ExecType,  std::string(1, static_cast<char>(view.exec_type)));
    er_msg.set(Tag::OrdStatus, std::string(1, static_cast<char>(view.ord_status)));
    er_msg.set(Tag::Symbol,    std::string{view.symbol});
    er_msg.set(Tag::Side,      std::string(1, static_cast<char>(view.side)));
    er_msg.set(Tag::CumQty,    std::string{view.cum_qty});
    er_msg.set(Tag::LeavesQty, std::string{view.leaves_qty});
    er_msg.set(Tag::ClOrdID,   cl_ord_id);

    if (view.has_order_qty) {
        er_msg.set(Tag::OrderQty, std::string{view.order_qty});
    }
    if (view.has_price) {
        er_msg.set(Tag::Price, std::string{view.price});
    }
    if (view.has_ord_type) {
        er_msg.set(Tag::OrdType, std::string(1, static_cast<char>(view.ord_type)));
    }

    send_fix_to_session(session, er_msg);

    // Clean up the routing map on terminal status. The original ClOrdID will
    // not appear on subsequent ERs once the order has reached one of these
    // states. Non-terminal statuses (PartiallyFilled, PendingCancel, etc.)
    // leave the entry in place so further fills can be routed.
    switch (view.ord_status) {
        case pubsub_itc_fw_app::OrdStatus::Filled:
        case pubsub_itc_fw_app::OrdStatus::Canceled:
        case pubsub_itc_fw_app::OrdStatus::Rejected:
        case pubsub_itc_fw_app::OrdStatus::Expired:
        case pubsub_itc_fw_app::OrdStatus::DoneForDay:
        case pubsub_itc_fw_app::OrdStatus::Replaced:
            cl_ord_id_to_session_.erase(map_it);
            break;
        default:
            break;
    }

    release_pdu_payload(message);
}

void FixGatewaySeqThread::on_timer_event(const std::string& name)
{
    for (auto& [id, session] : sessions_) {
        if (session.logon_timeout_timer_name() == name) {
            if (!session.session_established) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                           "FixGatewaySeqThread: connection {} logon timeout -- disconnecting",
                           id.get_value());
                disconnect_session(session, "logon timeout");
            }
            return;
        }
    }
}

void FixGatewaySeqThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{}

// -----------------------------------------------------------------------
// FIX session handlers
// -----------------------------------------------------------------------

void FixGatewaySeqThread::handle_logon(FixSession& session, const FixMessage& msg)
{
    cancel_timer(session.logon_timeout_timer_name());
    session.client_comp_id = msg.get(Tag::SenderCompID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} Logon from SenderCompID='{}'",
               session.conn_id.get_value(), session.client_comp_id);

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logon);
    reply.set(Tag::EncryptMethod, 0);

    const std::string& hbi = msg.get(Tag::HeartBtInt);
    reply.set(Tag::HeartBtInt, hbi.empty() ? 30 : std::stoi(hbi));

    send_fix_to_session(session, reply);
    session.session_established = true;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} FIX session established",
               session.conn_id.get_value());
}

void FixGatewaySeqThread::handle_heartbeat(FixSession& session,
                                            [[maybe_unused]] const FixMessage& msg)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
               "FixGatewaySeqThread: connection {} Heartbeat", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix_to_session(session, reply);
}

void FixGatewaySeqThread::handle_test_request(FixSession& session, const FixMessage& msg)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} TestRequest", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID
    send_fix_to_session(session, reply);
}

void FixGatewaySeqThread::handle_logout(FixSession& session, const FixMessage& msg)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} Logout: {}",
               session.conn_id.get_value(), msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix_to_session(session, reply);
    session.session_established = false;
    disconnect_session(session, "client sent Logout");
}

void FixGatewaySeqThread::handle_new_order_single(FixSession& session, const FixMessage& msg)
{
    const std::string& cl_ord_id  = msg.get(Tag::ClOrdID);
    const std::string& symbol     = msg.get(Tag::Symbol);
    const std::string& side_str   = msg.get(Tag::Side);
    const std::string& ord_type_str = msg.get(Tag::OrdType);
    const std::string& order_qty  = msg.get(Tag::OrderQty);
    const std::string& price_str  = msg.get(Tag::Price);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} NewOrderSingle ClOrdID={} Symbol={} Side={}",
               session.conn_id.get_value(), cl_ord_id, symbol, side_str);

    // Validate required fields.
    if (cl_ord_id.empty() || symbol.empty() || side_str.empty()
        || ord_type_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} NewOrderSingle missing required fields"
                   " -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // Record cl_ord_id -> session for ER routing.
    cl_ord_id_to_session_[cl_ord_id] = session.conn_id;

    // Build the DSL struct. All string fields use string_view pointing into
    // the FIX message -- safe because the struct is only live for this call.
    pubsub_itc_fw_app::NewOrderSingle nos{};
    nos.cl_ord_id     = cl_ord_id;
    nos.symbol        = symbol;
    nos.side          = static_cast<pubsub_itc_fw_app::Side>(side_str[0]);
    nos.ord_type      = static_cast<pubsub_itc_fw_app::OrdType>(ord_type_str[0]);
    nos.transact_time = 0; // TODO: parse SendingTime (tag 52) to epoch nanos
    nos.order_qty     = order_qty;

    if (!price_str.empty()) {
        nos.has_price = true;
        nos.price     = price_str;
    }

    const std::string& tif_str = msg.get(Tag::TimeInForce);
    if (!tif_str.empty()) {
        nos.has_time_in_force = true;
        nos.time_in_force     = static_cast<pubsub_itc_fw_app::TimeInForce>(tif_str[0]);
    }

    // Forward the encoded PDU to both sequencer instances.
    // The sequencer will wrap it in a SequencedMessage envelope.
    forward_pdu_to_sequencers(
        static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::NewOrderSingle), nos);
}

void FixGatewaySeqThread::handle_order_cancel_request(FixSession& session,
                                                       const FixMessage& msg)
{
    const std::string& cl_ord_id      = msg.get(Tag::ClOrdID);
    const std::string& orig_cl_ord_id = msg.get(Tag::OrigClOrdID);
    const std::string& symbol         = msg.get(Tag::Symbol);
    const std::string& side_str       = msg.get(Tag::Side);
    const std::string& order_qty      = msg.get(Tag::OrderQty);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} OrderCancelRequest ClOrdID={} "
               "OrigClOrdID={} Symbol={}",
               session.conn_id.get_value(), cl_ord_id, orig_cl_ord_id, symbol);

    if (cl_ord_id.empty() || orig_cl_ord_id.empty() || symbol.empty()
        || side_str.empty() || order_qty.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: connection {} OrderCancelRequest missing required "
                   "fields -- dropping",
                   session.conn_id.get_value());
        return;
    }

    // Record cl_ord_id -> session for ER routing of the cancel acknowledgement.
    cl_ord_id_to_session_[cl_ord_id] = session.conn_id;

    pubsub_itc_fw_app::OrderCancelRequest ocr{};
    ocr.orig_cl_ord_id = orig_cl_ord_id;
    ocr.cl_ord_id      = cl_ord_id;
    ocr.symbol         = symbol;
    ocr.side           = static_cast<pubsub_itc_fw_app::Side>(side_str[0]);
    ocr.transact_time  = 0; // TODO: parse SendingTime
    ocr.order_qty      = order_qty;

    forward_pdu_to_sequencers(
        static_cast<int16_t>(pubsub_itc_fw_app::Topics::TopicsTag::OrderCancelRequest), ocr);
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

void FixGatewaySeqThread::disconnect_session(FixSession& session, const std::string& reason)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: disconnecting connection {}: {}",
               session.conn_id.get_value(), reason);

    pubsub_itc_fw::ReactorControlCommand cmd(
        pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(cmd);
}

void FixGatewaySeqThread::send_fix_to_session(FixSession& session, FixMessage& msg)
{
    const std::string wire = serialiser_.serialise(msg, session.outbound_seq_num++);
    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
}

// -----------------------------------------------------------------------
// PDU forwarding
// -----------------------------------------------------------------------

// forward_pdu_to_sequencers is a template so it can accept any DSL message
// struct. It calls send_pdu on the primary sequencer connection. send_pdu
// handles all slab allocation, PduHeader framing, and two-pass encode/write
// internally.
//
// If the sequencer connection is not currently established (e.g. not yet
// reconnected after a failure), the PDU is dropped and a Warning is logged.
//
// The plural form of the function name is intentional: dual-publish to a
// follower will return when the leader-follower protocol is implemented,
// and the function will then fan out again. Until then there is one target.

} // namespace sample_fix_gateway_seq
