// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/MillisecondClock.hpp>
#include <pubsub_itc_fw/OutboundConnectionManager.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpConnector.hpp>

namespace pubsub_itc_fw {

OutboundConnectionManager::OutboundConnectionManager(int epoll_fd,
                                                     const ReactorConfiguration& config,
                                                     ExpandableSlabAllocator& inbound_allocator,
                                                     const ServiceRegistry& service_registry,
                                                     ThreadLookupInterface& thread_lookup,
                                                     QuillLogger& logger)
    : epoll_fd_(epoll_fd)
    , config_(config)
    , inbound_allocator_(inbound_allocator)
    , service_registry_(service_registry)
    , thread_lookup_(thread_lookup)
    , logger_(logger)
{
}

void OutboundConnectionManager::process_connect_command(const ReactorControlCommand& command,
                                                         ConnectionID id)
{
    const std::string& service_name = command.service_name_;

    auto [endpoints, lookup_error] = service_registry_.lookup(service_name);
    if (!lookup_error.empty()) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "OutboundConnectionManager::process_connect_command: unknown service '{}'",
            service_name);
        auto* thread = thread_lookup_.get_fast_path_thread(command.requesting_thread_id_);
        if (thread != nullptr) {
            thread->get_queue().enqueue(
                EventMessage::create_connection_failed_event(
                    fmt::format("Unknown service: {}", service_name)));
        }
        return;
    }

    const NetworkEndpointConfiguration& primary = endpoints.primary;

    auto [addr, addr_error] = InetAddress::create(primary.host, primary.port);
    if (!addr) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "OutboundConnectionManager::process_connect_command: failed to resolve {}:{} for "
            "service '{}' -- {}, will retry in {}ms",
            primary.host, primary.port, service_name, addr_error,
            config_.connect_retry_interval_.count());
        pending_retries_[service_name] = PendingRetry(
            command,
            std::chrono::steady_clock::now() + config_.connect_retry_interval_);
        return;
    }

    auto connector = std::make_unique<TcpConnector>();
    auto [connected_immediately, connect_error] = connector->connect(*addr);

    if (!connect_error.empty()) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "OutboundConnectionManager::process_connect_command: connect() to {}:{} for "
            "service '{}' failed -- {}, will retry in {}ms",
            primary.host, primary.port, service_name, connect_error,
            config_.connect_retry_interval_.count());
        pending_retries_[service_name] = PendingRetry(
            command,
            std::chrono::steady_clock::now() + config_.connect_retry_interval_);
        return;
    }

    const int fd = connector->get_fd();

    auto* target_thread = thread_lookup_.get_fast_path_thread(command.requesting_thread_id_);
    if (target_thread == nullptr) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Error,
            "OutboundConnectionManager::process_connect_command: requesting thread not found");
        return;
    }

    auto conn = std::make_unique<OutboundConnection>(
        id,
        command.requesting_thread_id_,
        service_name,
        endpoints,
        std::move(connector),
        inbound_allocator_,
        *target_thread);

    OutboundConnection* conn_ptr = conn.get();
    connections_[id]       = std::move(conn);
    connections_by_fd_[fd] = conn_ptr;

    if (connected_immediately) {
        on_connect_ready(*conn_ptr);
    } else {
        epoll_event ev{};
        ev.events  = EPOLLOUT | EPOLLERR;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "OutboundConnectionManager::process_connect_command: epoll_ctl ADD failed for fd {}",
                fd);
            teardown_connection(id, "epoll_ctl failed during connect", false);
            target_thread->get_queue().enqueue(
                EventMessage::create_connection_failed_event("epoll_ctl failed during connect"));
        }
    }
}

void OutboundConnectionManager::on_connect_ready(OutboundConnection& conn)
{
    auto [connected, error] = conn.connector()->finish_connect();

    if (!connected) {
        if (!error.empty()) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning,
                "OutboundConnectionManager::on_connect_ready: finish_connect failed for "
                "service '{}': {}",
                conn.service_name(), error);

            const NetworkEndpointConfiguration& secondary = conn.endpoints().secondary;
            if (!conn.is_trying_secondary() && secondary.port != 0) {
                PUBSUB_LOG(logger_, FwLogLevel::Info,
                    "OutboundConnectionManager::on_connect_ready: retrying service '{}' on "
                    "secondary {}:{}",
                    conn.service_name(), secondary.host, secondary.port);

                auto [addr, addr_error] = InetAddress::create(secondary.host, secondary.port);
                if (!addr) {
                    PUBSUB_LOG(logger_, FwLogLevel::Error,
                        "OutboundConnectionManager::on_connect_ready: failed to resolve "
                        "secondary {}:{} — {}",
                        secondary.host, secondary.port, addr_error);
                    teardown_connection(conn.id(), addr_error, false);
                    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
                    if (thread != nullptr) {
                        thread->get_queue().enqueue(
                            EventMessage::create_connection_failed_event(addr_error));
                    }
                    return;
                }

                const int old_fd = conn.get_fd();
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, old_fd, nullptr);
                connections_by_fd_.erase(old_fd);

                auto new_connector = std::make_unique<TcpConnector>();
                auto [connected_immediately, connect_error] = new_connector->connect(*addr);

                if (!connect_error.empty()) {
                    PUBSUB_LOG(logger_, FwLogLevel::Error,
                        "OutboundConnectionManager::on_connect_ready: secondary connect() "
                        "failed for service '{}': {}",
                        conn.service_name(), connect_error);
                    teardown_connection(conn.id(), connect_error, false);
                    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
                    if (thread != nullptr) {
                        thread->get_queue().enqueue(
                            EventMessage::create_connection_failed_event(connect_error));
                    }
                    return;
                }

                const int new_fd = new_connector->get_fd();
                conn.retry_with_secondary(std::move(new_connector));
                connections_by_fd_[new_fd] = &conn;

                if (connected_immediately) {
                    on_connect_ready(conn);
                } else {
                    epoll_event ev{};
                    ev.events  = EPOLLOUT | EPOLLERR;
                    ev.data.fd = new_fd;
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, new_fd, &ev);
                }
                return;
            }

            teardown_connection(conn.id(), error, false);
            PUBSUB_LOG(logger_, FwLogLevel::Warning,
                "OutboundConnectionManager::on_connect_ready: service '{}' failed, "
                "will retry in {}ms",
                conn.service_name(), config_.connect_retry_interval_.count());
            ReactorControlCommand retry_cmd{ReactorControlCommand::CommandTag::Connect};
            retry_cmd.requesting_thread_id_ = conn.requesting_thread_id();
            retry_cmd.service_name_         = conn.service_name();
            pending_retries_[conn.service_name()] = PendingRetry(
                retry_cmd,
                std::chrono::steady_clock::now() + config_.connect_retry_interval_);
        }
        // else still in progress — wait for next EPOLLOUT
        return;
    }

    const int fd = conn.get_fd();
    const ConnectionID conn_id = conn.id();

    auto socket = conn.connector()->get_connected_socket();
    auto disconnect_handler = [this, conn_id]() {
        teardown_connection(conn_id, "peer closed connection", true);
    };
    conn.on_connected(std::move(socket), std::move(disconnect_handler));

    if (config_.socket_send_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                     &config_.socket_send_buffer_size, sizeof(config_.socket_send_buffer_size));
    }
    if (config_.socket_receive_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                     &config_.socket_receive_buffer_size, sizeof(config_.socket_receive_buffer_size));
    }

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLERR;
    ev.data.fd = fd;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);

    PUBSUB_LOG(logger_, FwLogLevel::Info,
        "OutboundConnectionManager::on_connect_ready: connection {} to service '{}' established",
        conn.id().get_value(), conn.service_name());

    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
    if (thread != nullptr) {
        thread->get_queue().enqueue(
            EventMessage::create_connection_established_event(
                ConnectionID{conn.id().get_value(), conn.service_name()}));
    }
}

void OutboundConnectionManager::on_data_ready(OutboundConnection& conn)
{
    const ConnectionID id           = conn.id();
    const std::string  service_name = conn.service_name();

    auto [ok, error] = conn.parser()->receive();
    if (!ok) {
        const std::string reason = error.empty()
            ? fmt::format("peer closed connection on service '{}'", service_name)
            : fmt::format("parse error on service '{}': {}", service_name, error);

        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "OutboundConnectionManager::on_data_ready: {}", reason);

        teardown_connection(id, reason, true);
    }
}

void OutboundConnectionManager::on_write_ready(OutboundConnection& conn)
{
    auto [ok, error] = conn.framer()->continue_send();
    if (!ok) {
        const std::string reason = fmt::format(
            "send error on service '{}': {}", conn.service_name(), error);
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "OutboundConnectionManager::on_write_ready: {}", reason);
        teardown_connection(conn.id(), reason, true);
        return;
    }

    if (!conn.framer()->has_pending_data()) {
        conn.current_allocator()->deallocate(conn.current_slab_id(), conn.current_chunk_ptr());
        conn.clear_pending_send();

        const int fd = conn.get_fd();
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLERR;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    }
}

bool OutboundConnectionManager::process_send_pdu_command(const ReactorControlCommand& command)
{
    const ConnectionID cid = command.connection_id_;

    auto it = connections_.find(cid);
    if (it == connections_.end()) {
        return false;
    }

    OutboundConnection& conn = *it->second;

    if (!conn.is_established()) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "OutboundConnectionManager::process_send_pdu_command: outbound connection {} "
            "not yet established",
            cid.get_value());
        pending_send_ = command;
        return true;
    }

    if (conn.has_pending_send()) {
        pending_send_ = command;
        return true;
    }

    const uint32_t total_bytes =
        static_cast<uint32_t>(sizeof(PduHeader)) + command.pdu_byte_count_;
    const uint8_t* frame_ptr = static_cast<const uint8_t*>(command.pdu_chunk_ptr_);
    auto [ok, send_error] = conn.framer()->send_prebuilt(frame_ptr, total_bytes);

    if (!ok) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "OutboundConnectionManager::process_send_pdu_command: send error to '{}': {}",
            conn.service_name(), send_error);
        command.allocator_->deallocate(command.slab_id_, command.pdu_chunk_ptr_);
        teardown_connection(conn.id(), send_error, true);
        return true;
    }

    if (conn.framer()->has_pending_data()) {
        conn.set_pending_send(command.allocator_, command.slab_id_,
                              command.pdu_chunk_ptr_, total_bytes);
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLERR;
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    } else {
        command.allocator_->deallocate(command.slab_id_, command.pdu_chunk_ptr_);
    }

    return true;
}

bool OutboundConnectionManager::process_send_raw_command(const ReactorControlCommand& command)
{
    // OutboundConnection does not support raw-bytes connections in the current
    // implementation. Return false so the Reactor tries the inbound manager.
    [[maybe_unused]] const ConnectionID cid = command.connection_id_;
    return false;
}

bool OutboundConnectionManager::process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed)
{
    // OutboundConnection does not use the ProtocolHandlerInterface strategy —
    // it owns its parser and framer directly and is always PDU-based. A
    // CommitRawBytes command targeting an outbound connection ID is therefore
    // a no-op. Return true if we own the ID so the Reactor does not log a
    // spurious warning.
    [[maybe_unused]] int64_t ignored = bytes_consumed;
    return connections_.count(id) != 0;
}

bool OutboundConnectionManager::drain_pending_send()
{
    if (!pending_send_.has_value()) {
        return true;
    }

    const ReactorControlCommand command = *pending_send_;
    pending_send_.reset();

    const bool processed = process_send_pdu_command(command);
    if (!processed) {
        // Connection vanished while the command was stashed — deallocate.
        command.allocator_->deallocate(command.slab_id_, command.pdu_chunk_ptr_);
        return true;
    }

    // If still blocked, process_send_pdu_command will have re-stashed it.
    return !pending_send_.has_value();
}

bool OutboundConnectionManager::process_disconnect_command(ConnectionID id)
{
    if (connections_.count(id) == 0) {
        return false;
    }
    teardown_connection(id, "disconnect requested by application thread", true);
    return true;
}

void OutboundConnectionManager::retry_failed_connections(
    const std::function<ConnectionID()>& next_id_fn)
{
    if (pending_retries_.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    // Collect due retries first to avoid modifying the map while iterating.
    std::vector<std::string> due;
    for (const auto& [service_name, retry] : pending_retries_) {
        if (now >= retry.retry_after) {
            due.push_back(service_name);
        }
    }

    for (const std::string& service_name : due) {
        auto it = pending_retries_.find(service_name);
        if (it == pending_retries_.end()) {
            continue;
        }
        ReactorControlCommand cmd = it->second.command;
        pending_retries_.erase(it);

        PUBSUB_LOG(logger_, FwLogLevel::Info,
            "OutboundConnectionManager::retry_failed_connections: retrying service '{}'",
            service_name);

        process_connect_command(cmd, next_id_fn());
    }
}

void OutboundConnectionManager::check_for_timed_out_connections()
{
    const auto now = MillisecondClock::now();

    // Phase 1: identify timed-out connecting connections.
    std::vector<ConnectionID> timed_out;
    for (const auto& [id, conn] : connections_) {
        if (conn->is_connecting()) {
            const auto elapsed = now - conn->connect_started_at();
            if (elapsed > config_.connect_timeout) {
                timed_out.push_back(id);
            }
        }
    }

    // Phase 2: tear down each timed-out connection.
    for (const ConnectionID& id : timed_out) {
        auto it = connections_.find(id);
        if (it == connections_.end()) {
            continue;
        }
        const std::string reason = fmt::format(
            "connect timeout after {}ms for service '{}'",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                config_.connect_timeout).count(),
            it->second->service_name());

        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "OutboundConnectionManager::check_for_timed_out_connections: {}", reason);

        const ThreadID requesting_thread_id = it->second->requesting_thread_id();
        teardown_connection(id, reason, false);

        auto* thread = thread_lookup_.get_fast_path_thread(requesting_thread_id);
        if (thread != nullptr) {
            thread->get_queue().enqueue(
                EventMessage::create_connection_failed_event(reason));
        }
    }
}

OutboundConnection* OutboundConnectionManager::find_by_fd(int fd) const
{
    auto it = connections_by_fd_.find(fd);
    return (it != connections_by_fd_.end()) ? it->second : nullptr;
}

OutboundConnection* OutboundConnectionManager::find_by_id(ConnectionID id) const
{
    auto it = connections_.find(id);
    return (it != connections_.end()) ? it->second.get() : nullptr;
}

void OutboundConnectionManager::teardown_connection(ConnectionID id,
                                                     const std::string& reason,
                                                     bool deliver_lost_event)
{
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }

    OutboundConnection& conn = *it->second;

    PUBSUB_LOG(logger_, FwLogLevel::Info,
        "OutboundConnectionManager::teardown_connection: connection {} service '{}': {}",
        id.get_value(), conn.service_name(), reason);

    // Free any in-flight outbound slab chunk.
    if (conn.has_pending_send()) {
        conn.current_allocator()->deallocate(conn.current_slab_id(), conn.current_chunk_ptr());
        conn.clear_pending_send();
    }

    // Clear pending_send_ if it refers to this connection.
    if (pending_send_.has_value() && pending_send_->connection_id_ == id) {
        pending_send_->allocator_->deallocate(
            pending_send_->slab_id_, pending_send_->pdu_chunk_ptr_);
        pending_send_.reset();
    }

    // Deregister from epoll and remove from fd map.
    const int fd = conn.get_fd();
    if (fd != -1) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_by_fd_.erase(fd);
    }

    // Deliver ConnectionLost if requested and the connection was established.
    if (deliver_lost_event && conn.is_established()) {
        auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
        if (thread != nullptr) {
            thread->get_queue().enqueue(
                EventMessage::create_connection_lost_event(id, reason));
        }
    }

    connections_.erase(it);
}

} // namespace pubsub_itc_fw
