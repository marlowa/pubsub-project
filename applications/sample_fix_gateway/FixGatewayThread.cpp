// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewayThread.hpp"

#include <cstring>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sample_fix_gateway {

static pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static pubsub_itc_fw::AllocatorConfiguration make_allocator_config() {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name = "FixGatewayPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools = 1;
    return cfg;
}

FixGatewayThread::FixGatewayThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                   pubsub_itc_fw::Reactor& reactor, const FixGatewayConfiguration& config)
    : ApplicationThread(token, logger, reactor, "FixGatewayThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , serialiser_(config.sender_comp_id, config.default_target_comp_id) {}

void FixGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    sessions_.emplace(std::piecewise_construct, std::forward_as_tuple(id), std::forward_as_tuple(id, [this, id](const FixMessage& msg) {
                          auto it = sessions_.find(id);
                          if (it == sessions_.end()) {
                              return;
                          }
                          FixSession& session = it->second;
                          const std::string& type = msg.msg_type();

                          if (type == MsgType::Logon) {
                              handle_logon(session, msg);
                          } else if (!session.session_established) {
                              // Any non-Logon message before session is established is a
                              // protocol violation -- disconnect immediately.
                              PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                                         "FixGatewayThread: connection {} received MsgType='{}' before Logon -- disconnecting", id.get_value(), type);
                              disconnect_session(session, "first message was not Logon");
                          } else if (type == MsgType::Heartbeat) {
                              handle_heartbeat(session, msg);
                          } else if (type == MsgType::TestRequest) {
                              handle_test_request(session, msg);
                          } else if (type == MsgType::Logout) {
                              handle_logout(session, msg);
                          } else if (type == MsgType::NewOrderSingle) {
                              handle_new_order_single(session, msg);
                          } else {
                              PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} ignoring MsgType='{}'", id.get_value(),
                                         type);
                          }
                      }));

    // Start the logon timeout timer for this session.
    start_one_off_timer(sessions_.at(id).logon_timeout_timer_name(), config_.logon_timeout);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewayThread: connection {} established, waiting for Logon "
               "(timeout {}s) -- active sessions: {}",
               id.get_value(), config_.logon_timeout.count(), sessions_.size());
}

void FixGatewayThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        // Cancel the logon timeout timer if it is still running.
        // cancel_timer is a no-op if the timer has already fired or been cancelled.
        cancel_timer(it->second.logon_timeout_timer_name());
        sessions_.erase(it);
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} lost: {} -- active sessions: {}", id.get_value(), reason,
               sessions_.size());
}

void FixGatewayThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) {
    const pubsub_itc_fw::ConnectionID conn_id = message.connection_id();
    const uint8_t* data = message.payload();
    const int available = message.payload_size();

    if (data == nullptr || available <= 0) {
        return;
    }

    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewayThread::on_raw_socket_message: no session for connection {}",
                   conn_id.get_value());
        commit_raw_bytes(conn_id, static_cast<int64_t>(available));
        return;
    }

    FixSession& session = it->second;

    if (!session.preamble_verified) {
        // Check incoming bytes against the expected preamble as they arrive.
        // We compare only as many bytes as we have received so far -- TCP
        // fragmentation may deliver the preamble across multiple packets.
        // We reject immediately on any mismatch so that non-FIX clients
        // (e.g. telnet, port scanners) are disconnected without waiting for
        // a full preamble's worth of bytes to accumulate.
        const std::size_t bytes_to_check = std::min(static_cast<std::size_t>(available), expected_preamble.size());

        if (std::memcmp(data, expected_preamble.data(), bytes_to_check) != 0) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewayThread: connection {} invalid FIX preamble -- disconnecting",
                       conn_id.get_value());
            disconnect_session(session, "invalid FIX preamble");
            commit_raw_bytes(conn_id, static_cast<int64_t>(available));
            return;
        }

        if (static_cast<std::size_t>(available) < expected_preamble.size()) {
            // Matches so far but incomplete -- wait for more bytes.
            // The logon timeout handles the case where no more bytes arrive.
            commit_raw_bytes(conn_id, 0);
            return;
        }

        session.preamble_verified = true;

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} FIX preamble verified", conn_id.get_value());
    }

    session.parser.feed(data, available);
    commit_raw_bytes(conn_id, static_cast<int64_t>(available));
}

void FixGatewayThread::on_timer_event(const std::string& name) {
    // Find the session whose logon timeout timer fired.
    for (auto& [id, session] : sessions_) {
        if (session.logon_timeout_timer_name() == name) {
            if (!session.session_established) {
                PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "FixGatewayThread: connection {} logon timeout -- disconnecting", id.get_value());
                disconnect_session(session, "logon timeout");
            }
            return;
        }
    }
}

void FixGatewayThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& message) {}

void FixGatewayThread::handle_logon(FixSession& session, const FixMessage& msg) {
    // Cancel the logon timeout -- the client has logged in successfully.
    cancel_timer(session.logon_timeout_timer_name());

    session.client_comp_id = msg.get(Tag::SenderCompID);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} Logon from SenderCompID='{}'", session.conn_id.get_value(),
               session.client_comp_id);

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logon);
    reply.set(Tag::EncryptMethod, 0);

    const std::string& hbi = msg.get(Tag::HeartBtInt);
    reply.set(Tag::HeartBtInt, hbi.empty() ? 30 : std::stoi(hbi));

    send_fix_to_session(session, reply);
    session.session_established = true;

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} FIX session established", session.conn_id.get_value());
}

void FixGatewayThread::handle_heartbeat(FixSession& session, [[maybe_unused]] const FixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Debug, "FixGatewayThread: connection {} Heartbeat", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix_to_session(session, reply);
}

void FixGatewayThread::handle_test_request(FixSession& session, const FixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} TestRequest", session.conn_id.get_value());

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID
    send_fix_to_session(session, reply);
}

void FixGatewayThread::handle_logout(FixSession& session, const FixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} Logout: {}", session.conn_id.get_value(), msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix_to_session(session, reply);

    session.session_established = false;
    disconnect_session(session, "client sent Logout");
}

void FixGatewayThread::handle_new_order_single(FixSession& session, const FixMessage& msg) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "FixGatewayThread: connection {} NewOrderSingle "
               "ClOrdID={} Symbol={} Side={} Qty={} Price={}",
               session.conn_id.get_value(), msg.get(Tag::ClOrdID), msg.get(Tag::Symbol), msg.get(Tag::Side), msg.get(Tag::OrderQty), msg.get(Tag::Price));

    const std::string order_id = generate_order_id(session);
    const std::string exec_id = generate_exec_id(session);

    FixMessage er;
    er.set(Tag::MsgType, MsgType::ExecutionReport);
    er.set(Tag::OrderID, order_id);
    er.set(Tag::ExecID, exec_id);
    er.set(Tag::ExecType, "F");  // Trade (filled)
    er.set(Tag::OrdStatus, "2"); // Filled
    er.set(Tag::Symbol, msg.get(Tag::Symbol));
    er.set(Tag::Side, msg.get(Tag::Side));
    er.set(Tag::OrderQty, msg.get(Tag::OrderQty));
    er.set(Tag::Price, msg.get(Tag::Price));
    er.set(Tag::CumQty, msg.get(Tag::OrderQty));
    er.set(Tag::LeavesQty, "0");
    er.set(Tag::ClOrdID, msg.get(Tag::ClOrdID));

    send_fix_to_session(session, er);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: connection {} sent ExecutionReport OrderID={} ExecID={}",
               session.conn_id.get_value(), order_id, exec_id);
}

void FixGatewayThread::disconnect_session(FixSession& session, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "FixGatewayThread: disconnecting connection {}: {}", session.conn_id.get_value(), reason);

    pubsub_itc_fw::ReactorControlCommand cmd(pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = session.conn_id;
    get_reactor().enqueue_control_command(cmd);
}

void FixGatewayThread::send_fix_to_session(FixSession& session, FixMessage& msg) {
    const std::string wire = serialiser_.serialise(msg, session.outbound_seq_num++);
    send_raw(session.conn_id, wire.data(), static_cast<uint32_t>(wire.size()));
}

std::string FixGatewayThread::generate_order_id(FixSession& session) {
    return "ORD-" + std::to_string(session.conn_id.get_value()) + "-" + std::to_string(session.order_id_counter++);
}

std::string FixGatewayThread::generate_exec_id(FixSession& session) {
    return "EXEC-" + std::to_string(session.conn_id.get_value()) + "-" + std::to_string(session.exec_id_counter++);
}

} // namespace sample_fix_gateway
