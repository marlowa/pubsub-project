// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <optional>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/InboundConnectionManager.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PduProtocolHandler.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/RawBytesProtocolHandler.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>

namespace pubsub_itc_fw {

InboundConnectionManager::InboundConnectionManager(int epoll_fd,
                                                   const ReactorConfiguration& config,
                                                   ExpandableSlabAllocator& inbound_allocator,
                                                   ThreadLookupInterface& thread_lookup,
                                                   QuillLogger& logger)
    : epoll_fd_(epoll_fd)
    , config_(config)
    , inbound_allocator_(inbound_allocator)
    , thread_lookup_(thread_lookup)
    , logger_(logger)
{
}

void InboundConnectionManager::register_inbound_listener(NetworkEndpointConfiguration address,
                                                          ThreadID target_thread_id,
                                                          ProtocolType protocol_type,
                                                          int64_t raw_buffer_capacity)
{
    InboundListener listener;
    listener.configuration.address              = std::move(address);
    listener.configuration.target_thread_id     = target_thread_id;
    listener.configuration.protocol_type        = protocol_type;
    listener.configuration.raw_buffer_capacity  = raw_buffer_capacity;
    inbound_listeners_staging_.push_back(std::move(listener));
}

bool InboundConnectionManager::initialize_listeners()
{
    for (auto& listener : inbound_listeners_staging_) {
        auto [addr, addr_error] = InetAddress::create(listener.configuration.address.host,
                                                       listener.configuration.address.port);
        if (!addr) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "InboundConnectionManager::initialize_listeners: failed to resolve {}:{} — {}",
                listener.configuration.address.host, listener.configuration.address.port, addr_error);
            return false;
        }

        auto [acceptor, accept_error] = TcpAcceptor::create(*addr, /*backlog=*/4);
        if (!acceptor) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "InboundConnectionManager::initialize_listeners: failed to create acceptor on {}:{} — {}",
                listener.configuration.address.host, listener.configuration.address.port, accept_error);
            return false;
        }

        const int listen_fd = acceptor->get_listening_file_descriptor();
        listener.acceptor = std::move(acceptor);

        PUBSUB_LOG(logger_, FwLogLevel::Info,
            "InboundConnectionManager::initialize_listeners: listening on {}:{}",
            listener.configuration.address.host, listener.configuration.address.port);

        inbound_listeners_[listen_fd] = std::move(listener);

        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = listen_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "InboundConnectionManager::initialize_listeners: epoll_ctl ADD failed — {}",
                StringUtils::get_errno_string());
            return false;
        }
    }
    inbound_listeners_staging_.clear();
    return true;
}

void InboundConnectionManager::on_accept(InboundListener& listener, ConnectionID id)
{
    auto [socket, peer_addr, error] = listener.acceptor->accept_connection();

    if (!socket) {
        if (!error.empty()) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                "InboundConnectionManager::on_accept: accept_connection failed on port {} — {}",
                listener.configuration.address.port, error);
        }
        return; // EAGAIN — no connection waiting
    }

    const std::string peer_desc = peer_addr
        ? fmt::format("{}:{}", peer_addr->get_ip_address_string(), peer_addr->get_port())
        : "unknown peer";

    // Enforce the one-connection rule for FrameworkPdu listeners only.
    // RawBytes listeners (e.g. FIX gateways) accept multiple concurrent connections.
    if (listener.has_connection() &&
        listener.configuration.protocol_type == ProtocolType::FrameworkPdu) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
            "InboundConnectionManager::on_accept: listener on port {} already has an established "
            "connection (connection id {}). Rejecting new connection attempt from {}. "
            "This indicates a framework misuse by the connecting application -- "
            "only one peer should connect to a given framework listener port.",
            listener.configuration.address.port,
            listener.current_connection_id.get_value(),
            peer_desc);
        socket->close();
        return;
        }

    const int fd = socket->get_file_descriptor();

    auto* target_thread = thread_lookup_.get_fast_path_thread(listener.configuration.target_thread_id);
    if (target_thread == nullptr) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "InboundConnectionManager::on_accept: target thread {} not found — "
            "rejecting connection from {}",
            listener.configuration.target_thread_id.get_value(), peer_desc);
        socket->close();
        return;
    }

    auto disconnect_handler = [this, id]() {
        teardown_connection(id, "peer closed connection", true);
    };

    std::unique_ptr<ProtocolHandlerInterface> handler;
    if (listener.configuration.protocol_type == ProtocolType::RawBytes) {
        handler = std::make_unique<RawBytesProtocolHandler>(id,
            *socket,
            *target_thread,
            listener.configuration.raw_buffer_capacity,
            std::move(disconnect_handler),
            logger_);
    } else {
        handler = std::make_unique<PduProtocolHandler>(
            *socket,
            *target_thread,
            inbound_allocator_,
            std::move(disconnect_handler),
            logger_);
    }

    auto conn = std::make_unique<InboundConnection>(
        id,
        std::move(socket),
        listener.configuration.target_thread_id,
        std::move(handler),
        peer_desc);

    InboundConnection* conn_ptr = conn.get();
    connections_[id]       = std::move(conn);
    connections_by_fd_[fd] = conn_ptr;
    listener.current_connection_id = id;

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLERR;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "InboundConnectionManager::on_accept: epoll_ctl ADD failed for "
            "connection from {} — {}",
            peer_desc, StringUtils::get_errno_string());
        teardown_connection(id, "epoll_ctl failed", false);
        return;
    }

    PUBSUB_LOG(logger_, FwLogLevel::Info,
        "InboundConnectionManager::on_accept: accepted connection {} from {} on port {}",
        id.get_value(), peer_desc, listener.configuration.address.port);

    target_thread->get_queue().enqueue(
        EventMessage::create_connection_established_event(id));
}

void InboundConnectionManager::on_data_ready(InboundConnection& conn)
{
    conn.handle_read();
    // Note: handle_read() may invoke the disconnect handler synchronously,
    // which calls teardown_connection() and destroys conn.
    // Any access to conn after this point would be a use-after-free error.
}

void InboundConnectionManager::on_write_ready(InboundConnection& conn)
{
    const ConnectionID id = conn.id();

    conn.handler()->continue_send();

    // continue_send() may have invoked the disconnect handler synchronously
    // on an unrecoverable send error, destroying conn. Check whether the
    // connection still exists before touching it again.
    if (connections_.find(id) == connections_.end()) {
        return;
    }

    if (!conn.handler()->has_pending_send()) {
        const int fd = conn.get_fd();
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLERR;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    }
}

void InboundConnectionManager::teardown_connection(ConnectionID id,
                                                    const std::string& reason,
                                                    bool deliver_lost_event)
{
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }

    InboundConnection& conn = *it->second;
    const ThreadID target_thread_id = conn.target_thread_id();

    PUBSUB_LOG(logger_, FwLogLevel::Info,
        "InboundConnectionManager::teardown_connection: connection {} from '{}': {}",
        id.get_value(), conn.peer_description(), reason);

    // Free any in-flight outbound slab chunk via the handler.
    if (conn.handler()->has_pending_send()) {
        conn.handler()->deallocate_pending_send();
    }

    // Clear pending_send_ if it refers to this connection.
    if (pending_send_.has_value() && pending_send_->connection_id_ == id) {
        pending_send_->allocator_->deallocate(
            pending_send_->slab_id_, pending_send_->pdu_chunk_ptr_);
        pending_send_.reset();
    }

    // Deregister from epoll and fd map.
    const int fd = conn.get_fd();
    if (fd != -1) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_by_fd_.erase(fd);
    }

    // Clear the listener's current connection so it can accept again.
    // Only tracked for FrameworkPdu listeners.
    for (auto& [listen_fd, listener] : inbound_listeners_) {
        if (listener.current_connection_id == id &&
            listener.configuration.protocol_type == ProtocolType::FrameworkPdu) {
            listener.current_connection_id = ConnectionID{};
            break;
            }
    }

    // Deliver ConnectionLost if requested.
    if (deliver_lost_event) {
        auto* thread = thread_lookup_.get_fast_path_thread(target_thread_id);
        if (thread != nullptr) {
            thread->get_queue().enqueue(
                EventMessage::create_connection_lost_event(id, reason));
        }
    }

    connections_.erase(it);
}

void InboundConnectionManager::check_for_inactive_connections()
{
    const auto now = std::chrono::steady_clock::now();

    // Phase 1: identify connections that have exceeded the idle timeout.
    // Collect into a vector first to avoid iterator invalidation when
    // teardown_connection() modifies connections_.
    std::vector<ConnectionID> idle_connections;
    for (const auto& [id, conn] : connections_) {
        const auto elapsed = now - conn->last_activity_time();
        if (elapsed > config_.socket_maximum_inactivity_interval_) {
            idle_connections.push_back(id);
        }
    }

    // Phase 2: tear down each idle connection.
    for (const ConnectionID& id : idle_connections) {
        auto it = connections_.find(id);
        if (it == connections_.end()) {
            continue; // already torn down by a concurrent event in this loop
        }
        const std::string reason = fmt::format(
            "connection from '{}' closed: no data received for {}s",
            it->second->peer_description(),
            std::chrono::duration_cast<std::chrono::seconds>(
                config_.socket_maximum_inactivity_interval_).count());

        PUBSUB_LOG(logger_, FwLogLevel::Info,
            "InboundConnectionManager::check_for_inactive_connections: {}", reason);

        teardown_connection(id, reason, true);
    }
}

bool InboundConnectionManager::process_send_pdu_command(const ReactorControlCommand& command)
{
    const ConnectionID cid = command.connection_id_;

    auto it = connections_.find(cid);
    if (it == connections_.end()) {
        return false;
    }

    InboundConnection& conn = *it->second;

    if (conn.handler()->has_pending_send()) {
        pending_send_ = command;
        return true;
    }

    const uint32_t total_bytes =
        static_cast<uint32_t>(sizeof(PduHeader)) + command.pdu_byte_count_;

    // The handler takes ownership of the slab bookkeeping from this point.
    // It stores allocator, slab_id, and chunk_ptr internally and will
    // deallocate on completion or teardown.
    conn.handler()->send_prebuilt(command.allocator_, command.slab_id_,
                                  command.pdu_chunk_ptr_, total_bytes);

    if (conn.handler()->has_pending_send()) {
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLERR;
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    }

    return true;
}

bool InboundConnectionManager::process_send_raw_command(const ReactorControlCommand& command)
{
    const ConnectionID cid = command.connection_id_;

    auto it = connections_.find(cid);
    if (it == connections_.end()) {
        return false;
    }

    InboundConnection& conn = *it->second;

    if (conn.handler()->has_pending_send()) {
        pending_send_ = command;
        return true;
    }

    conn.handler()->send_prebuilt(command.allocator_, command.slab_id_,
                                  command.raw_chunk_ptr_, command.raw_byte_count_);

    if (conn.handler()->has_pending_send()) {
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLOUT | EPOLLERR;
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    }

    return true;
}

bool InboundConnectionManager::process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed)
{
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return false;
    }
    it->second->handler()->commit_bytes(bytes_consumed);
    return true;
}

bool InboundConnectionManager::drain_pending_send()
{
    if (!pending_send_.has_value()) {
        return true;
    }

    const ReactorControlCommand command = *pending_send_;
    pending_send_.reset();

    bool processed = false;
    if (command.as_tag() == ReactorControlCommand::SendRaw) {
        processed = process_send_raw_command(command);
        if (!processed) {
            command.allocator_->deallocate(command.slab_id_, command.raw_chunk_ptr_);
            return true;
        }
    } else {
        processed = process_send_pdu_command(command);
        if (!processed) {
            // Connection vanished while the command was stashed — deallocate.
            command.allocator_->deallocate(command.slab_id_, command.pdu_chunk_ptr_);
            return true;
        }
    }

    // If still blocked, the relevant process_send_*_command will have re-stashed it.
    return !pending_send_.has_value();
}

bool InboundConnectionManager::process_disconnect_command(ConnectionID id)
{
    if (connections_.count(id) == 0) {
        return false;
    }
    teardown_connection(id, "disconnect requested by application thread", true);
    return true;
}

bool InboundConnectionManager::owns_connection(ConnectionID id) const
{
    return connections_.count(id) != 0;
}

InboundConnection* InboundConnectionManager::find_by_fd(int fd) const
{
    auto it = connections_by_fd_.find(fd);
    return (it != connections_by_fd_.end()) ? it->second : nullptr;
}

InboundListener* InboundConnectionManager::find_listener_by_fd(int fd)
{
    auto it = inbound_listeners_.find(fd);
    return (it != inbound_listeners_.end()) ? &it->second : nullptr;
}

uint16_t InboundConnectionManager::get_first_listener_port() const
{
    if (inbound_listeners_.empty()) {
        return 0;
    }
    const int listen_fd = inbound_listeners_.begin()->first;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == -1) {
        return 0;
    }
    if (addr.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
    return 0;
}

} // namespace pubsub_itc_fw
