// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewaySeqThread.hpp"

#include <cstring>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
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
    , sequencer_secondary_conn_id_{}
{}

void FixGatewaySeqThread::on_app_ready_event()
{
    connect_to_service("sequencer_primary");
    connect_to_service("sequencer_secondary");
}

void FixGatewaySeqThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    // TODO: use ServiceRegistry to distinguish FIX client connections from
    // sequencer connections and store sequencer_primary_conn_id_ /
    // sequencer_secondary_conn_id_ accordingly. For now all connections are
    // logged and FIX sessions are created for inbound connections only when
    // raw bytes arrive.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} established", id.get_value());
}

void FixGatewaySeqThread::on_connection_lost(pubsub_itc_fw::ConnectionID id,
                                              const std::string& reason)
{
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        cancel_timer(it->second.logon_timeout_timer_name());
        sessions_.erase(it);
    }

    if (id == sequencer_primary_conn_id_) {
        sequencer_primary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: primary sequencer connection {} lost: {}",
                   id.get_value(), reason);
    } else if (id == sequencer_secondary_conn_id_) {
        sequencer_secondary_conn_id_ = pubsub_itc_fw::ConnectionID{};
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: secondary sequencer connection {} lost: {}",
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
               "FixGatewaySeqThread: {} raw bytes received on connection {}",
               available, conn_id.get_value());

    // Create a session on first data from this connection if not already present.
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
                               "FixGatewaySeqThread: connection {} MsgType='{}' before Logon -- disconnecting",
                               conn_id.get_value(), type);
                    disconnect_session(session, "first message was not Logon");
                } else if (type == MsgType::Heartbeat) {
                    handle_heartbeat(session, msg);
                } else if (type == MsgType::TestRequest) {
                    handle_test_request(session, msg);
                } else if (type == MsgType::Logout) {
                    handle_logout(session, msg);
                } else if (type == MsgType::NewOrderSingle) {
                    handle_new_order_single(session, msg);
                } else if (type == MsgType::OrderCancelRequest) {
                    handle_order_cancel_request(session, msg);
                } else {
                    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                               "FixGatewaySeqThread: connection {} ignoring MsgType='{}'",
                               conn_id.get_value(), type);
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
    // ExecutionReport PDU received from the sequencer (forwarded from the ME).
    // TODO: decode ExecutionReport PDU using fix_equity_orders.hpp.
    // Look up cl_ord_id in cl_ord_id_to_session_. If absent, log and drop.
    // Otherwise serialise a FIX ExecutionReport and send_raw to the client.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: ER PDU received on connection {} -- decode TODO",
               message.connection_id().get_value());
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
    const std::string& cl_ord_id = msg.get(Tag::ClOrdID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} NewOrderSingle ClOrdID={} Symbol={} Side={}",
               session.conn_id.get_value(), cl_ord_id,
               msg.get(Tag::Symbol), msg.get(Tag::Side));

    // Record cl_ord_id -> session for ER routing.
    if (!cl_ord_id.empty()) {
        cl_ord_id_to_session_[cl_ord_id] = session.conn_id;
    }

    // TODO: encode as fix_equity_orders::NewOrderSingle PDU using the
    // generated fix_equity_orders.hpp header and forward to both sequencers
    // via forward_to_sequencers(). For now log the stub.
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} NewOrderSingle PDU encoding -- TODO",
               session.conn_id.get_value());
}

void FixGatewaySeqThread::handle_order_cancel_request(FixSession& session,
                                                       const FixMessage& msg)
{
    const std::string& cl_ord_id = msg.get(Tag::ClOrdID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} OrderCancelRequest ClOrdID={} Symbol={}",
               session.conn_id.get_value(), cl_ord_id, msg.get(Tag::Symbol));

    // TODO: encode as fix_equity_orders::OrderCancelRequest PDU and forward
    // to both sequencers via forward_to_sequencers().
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewaySeqThread: connection {} OrderCancelRequest PDU encoding -- TODO",
               session.conn_id.get_value());
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

void FixGatewaySeqThread::forward_to_sequencers(const void* data, uint32_t size)
{
    if (sequencer_primary_conn_id_.get_value() != 0) {
        send_raw(sequencer_primary_conn_id_, data, size);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: primary sequencer not connected, PDU not forwarded");
    }

    if (sequencer_secondary_conn_id_.get_value() != 0) {
        send_raw(sequencer_secondary_conn_id_, data, size);
    } else {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "FixGatewaySeqThread: secondary sequencer not connected, PDU not forwarded");
    }
}

} // namespace sample_fix_gateway_seq
