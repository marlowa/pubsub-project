// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixGatewayThread.hpp"

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
    cfg.low_watermark  = 1;
    cfg.high_watermark = 64;
    return cfg;
}

static pubsub_itc_fw::AllocatorConfiguration make_allocator_config() {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name        = "FixGatewayPool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools    = 1;
    return cfg;
}

FixGatewayThread::FixGatewayThread(pubsub_itc_fw::QuillLogger& logger,
                                   pubsub_itc_fw::Reactor& reactor,
                                   std::string sender_comp_id,
                                   std::string target_comp_id)
    : ApplicationThread(logger, reactor, "FixGatewayThread",
                        pubsub_itc_fw::ThreadID{1},
                        make_queue_config(),
                        make_allocator_config(),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , parser_([this](const FixMessage& msg) {
        const std::string& type = msg.msg_type();

        if (type == MsgType::Logon) {
            handle_logon(msg);
        } else if (type == MsgType::Heartbeat) {
            handle_heartbeat(msg);
        } else if (type == MsgType::TestRequest) {
            handle_test_request(msg);
        } else if (type == MsgType::Logout) {
            handle_logout(msg);
        } else if (type == MsgType::NewOrderSingle) {
            handle_new_order_single(msg);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
                "FixGatewayThread: ignoring message type '{}'", type);
        }
    })
    , serialiser_(std::move(sender_comp_id), std::move(target_comp_id))
{
}

void FixGatewayThread::on_connection_established(pubsub_itc_fw::ConnectionID id)
{
    conn_id_           = id;
    outbound_seq_num_  = 1;
    order_id_counter_  = 1;
    exec_id_counter_   = 1;
    session_established_.store(false, std::memory_order_release);
    parser_.reset();

    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: FIX connection established, waiting for Logon");
}

void FixGatewayThread::on_connection_lost(pubsub_itc_fw::ConnectionID,
                                          const std::string& reason)
{
    session_established_.store(false, std::memory_order_release);
    parser_.reset();

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: connection lost: {}", reason);
}

void FixGatewayThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& message)
{
    const uint8_t* data = message.payload();
    const int available = message.payload_size();

    if (data == nullptr || available <= 0) {
        return;
    }

    parser_.feed(data, available);
    commit_raw_bytes(conn_id_, static_cast<int64_t>(available));
}

void FixGatewayThread::on_itc_message(
    [[maybe_unused]] const pubsub_itc_fw::EventMessage& message)
{
}

void FixGatewayThread::handle_logon(const FixMessage& msg)
{
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: received Logon");

    // Echo back a Logon with the same HeartBtInt.
    FixMessage reply;
    reply.set(Tag::MsgType,       MsgType::Logon);
    reply.set(Tag::EncryptMethod, 0);

    const std::string& hbi = msg.get(Tag::HeartBtInt);
    reply.set(Tag::HeartBtInt, hbi.empty() ? 30 : std::stoi(hbi));

    send_fix(reply);
    session_established_.store(true, std::memory_order_release);

    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: FIX session established");
}

void FixGatewayThread::handle_heartbeat([[maybe_unused]] const FixMessage& msg)
{
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Debug,
        "FixGatewayThread: received Heartbeat");

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    send_fix(reply);
}

void FixGatewayThread::handle_test_request(const FixMessage& msg)
{
    PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: received TestRequest");

    // Respond with Heartbeat carrying the TestReqID (tag 112).
    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Heartbeat);
    reply.set(112, msg.get(112)); // TestReqID
    send_fix(reply);
}

void FixGatewayThread::handle_logout(const FixMessage& msg)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: received Logout: {}",
        msg.get(Tag::Text));

    FixMessage reply;
    reply.set(Tag::MsgType, MsgType::Logout);
    send_fix(reply);

    session_established_.store(false, std::memory_order_release);

    // Disconnect.
    pubsub_itc_fw::ReactorControlCommand cmd(
        pubsub_itc_fw::ReactorControlCommand::CommandTag::Disconnect);
    cmd.connection_id_ = conn_id_;
    get_reactor().enqueue_control_command(cmd);
}

void FixGatewayThread::handle_new_order_single(const FixMessage& msg)
{
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: NewOrderSingle ClOrdID={} Symbol={} Side={} Qty={} Price={}",
        msg.get(Tag::ClOrdID),
        msg.get(Tag::Symbol),
        msg.get(Tag::Side),
        msg.get(Tag::OrderQty),
        msg.get(Tag::Price));

    const std::string order_id = generate_order_id();
    const std::string exec_id  = generate_exec_id();

    // Send a filled ExecutionReport.
    FixMessage er;
    er.set(Tag::MsgType,   MsgType::ExecutionReport);
    er.set(Tag::OrderID,   order_id);
    er.set(Tag::ExecID,    exec_id);
    er.set(Tag::ExecType,  "F"); // Trade (filled)
    er.set(Tag::OrdStatus, "2"); // Filled
    er.set(Tag::Symbol,    msg.get(Tag::Symbol));
    er.set(Tag::Side,      msg.get(Tag::Side));
    er.set(Tag::OrderQty,  msg.get(Tag::OrderQty));
    er.set(Tag::Price,     msg.get(Tag::Price));
    er.set(Tag::CumQty,    msg.get(Tag::OrderQty));
    er.set(Tag::LeavesQty, "0");
    er.set(Tag::ClOrdID,   msg.get(Tag::ClOrdID));

    send_fix(er);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
        "FixGatewayThread: sent ExecutionReport OrderID={} ExecID={}",
        order_id, exec_id);
}

void FixGatewayThread::send_fix(FixMessage& msg)
{
    const std::string wire = serialiser_.serialise(msg, outbound_seq_num_++);
    send_raw(conn_id_, wire.data(), static_cast<uint32_t>(wire.size()));
}

std::string FixGatewayThread::generate_order_id()
{
    return "ORD-" + std::to_string(order_id_counter_++);
}

std::string FixGatewayThread::generate_exec_id()
{
    return "EXEC-" + std::to_string(exec_id_counter_++);
}

} // namespace sample_fix_gateway
