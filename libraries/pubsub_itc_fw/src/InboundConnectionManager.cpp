// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <optional>
#include <vector>

#include <cstdint>
#include <cstddef>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/InboundConnectionManager.hpp>
#include <pubsub_itc_fw/InetAddress.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/PduProtocolHandler.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/RawBytesProtocolHandler.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpAcceptor.hpp>
#include <pubsub_itc_fw/TlsContext.hpp>
#include <pubsub_itc_fw/TlsListenerConfiguration.hpp>
#include <pubsub_itc_fw/TlsRawBytesProtocolHandler.hpp>

namespace pubsub_itc_fw {

InboundConnectionManager::InboundConnectionManager(int epoll_fd, const ReactorConfiguration& config, ExpandableSlabAllocator& inbound_allocator,
                                                   ThreadLookupInterface& thread_lookup, QuillLogger& logger)
    : epoll_fd_(epoll_fd), config_(config), inbound_allocator_(inbound_allocator), thread_lookup_(thread_lookup), logger_(logger) {}

void InboundConnectionManager::register_inbound_listener(NetworkEndpointConfiguration address, ThreadID target_thread_id, ProtocolType protocol_type,
                                                         int64_t raw_buffer_capacity, IdleTimeoutFlag idle_timeout) {
    InboundListener listener;
    listener.configuration.address = std::move(address);
    listener.configuration.target_thread_id = target_thread_id;
    listener.configuration.protocol_type = protocol_type;
    listener.configuration.raw_buffer_capacity = raw_buffer_capacity;
    listener.configuration.idle_timeout = idle_timeout;
    inbound_listeners_staging_.push_back(std::move(listener));
}

void InboundConnectionManager::register_inbound_tls_listener(NetworkEndpointConfiguration address, ThreadID target_thread_id,
                                                              int64_t raw_buffer_capacity, TlsListenerConfiguration tls_config) {
    InboundListener listener;
    listener.configuration.address = std::move(address);
    listener.configuration.target_thread_id = target_thread_id;
    listener.configuration.protocol_type = ProtocolType{ProtocolType::TlsRawBytes};
    listener.configuration.raw_buffer_capacity = raw_buffer_capacity;
    listener.configuration.tls = std::move(tls_config);
    inbound_listeners_staging_.push_back(std::move(listener));
}

bool InboundConnectionManager::initialize_listeners() {
    for (auto& listener : inbound_listeners_staging_) {
        auto [addr, addr_error] = InetAddress::create(listener.configuration.address.host, listener.configuration.address.port);
        if (!addr) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::initialize_listeners: failed to resolve {}:{} — {}",
                       listener.configuration.address.host, listener.configuration.address.port, addr_error);
            return false;
        }

        auto [acceptor, accept_error] = TcpAcceptor::create(*addr, /*backlog=*/4);
        if (!acceptor) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::initialize_listeners: failed to create acceptor on {}:{} — {}",
                       listener.configuration.address.host, listener.configuration.address.port, accept_error);
            return false;
        }

        if (listener.configuration.protocol_type == ProtocolType::TlsRawBytes) {
            if (!listener.configuration.tls.has_value()) {
                PUBSUB_LOG(logger_, FwLogLevel::Error,
                           "InboundConnectionManager::initialize_listeners: TlsRawBytes listener on {}:{} has no TLS configuration",
                           listener.configuration.address.host, listener.configuration.address.port);
                return false;
            }
            const TlsListenerConfiguration& tls_config = listener.configuration.tls.value();
            auto [tls_context, tls_error] = TlsContext::create_server(tls_config.certificate_path, tls_config.private_key_path,
                                                                       tls_config.ca_path, tls_config.require_client_certificate);
            if (tls_context == nullptr) {
                PUBSUB_LOG(logger_, FwLogLevel::Error,
                           "InboundConnectionManager::initialize_listeners: failed to create TlsContext for {}:{} -- {}",
                           listener.configuration.address.host, listener.configuration.address.port, tls_error);
                return false;
            }
            listener.tls_context = std::move(tls_context);
        }

        const int listen_fd = acceptor->get_listening_file_descriptor();
        listener.acceptor = std::move(acceptor);

        PUBSUB_LOG(logger_, FwLogLevel::Info, "InboundConnectionManager::initialize_listeners: listening on {}:{}", listener.configuration.address.host,
                   listener.configuration.address.port);

        inbound_listeners_[listen_fd] = std::move(listener);

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = listen_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::initialize_listeners: epoll_ctl ADD failed — {}",
                       StringUtils::get_errno_string());
            return false;
        }

        listener_fds_in_registration_order_.push_back(listen_fd);
    }
    inbound_listeners_staging_.clear();
    return true;
}

void InboundConnectionManager::on_accept(InboundListener& listener, ConnectionID id) {
    auto [socket, peer_addr, error] = listener.acceptor->accept_connection();
    if (!socket) {
        if (!error.empty()) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::on_accept: accept_connection failed on port {} -- {}",
                       listener.configuration.address.port, error);
        }
        return;
    }
    const std::string peer_desc = peer_addr ? fmt::format("{}:{}", peer_addr->get_ip_address_string(), peer_addr->get_port()) : "unknown peer";

    // Build the populated ConnectionID once. Every downstream reference to this
    // connection (handler, InboundConnection, map key, ConnectionEstablished
    // event, FrameworkPdu events stamped by PduParser) must use this object so
    // that service_name is consistently visible.
    const ConnectionID populated_id{id.get_value(), fmt::format("inbound:{}", listener.configuration.address.port)};

    const int fd = socket->get_file_descriptor();
    if (config_.socket_send_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &config_.socket_send_buffer_size, sizeof(config_.socket_send_buffer_size));
    }
    if (config_.socket_receive_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config_.socket_receive_buffer_size, sizeof(config_.socket_receive_buffer_size));
    }
    auto* target_thread = thread_lookup_.get_fast_path_thread(listener.configuration.target_thread_id);
    if (target_thread == nullptr) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
                   "InboundConnectionManager::on_accept: target thread {} not found -- "
                   "rejecting connection from {}",
                   listener.configuration.target_thread_id.get_value(), peer_desc);
        socket->close();
        return;
    }

    std::unique_ptr<ProtocolHandlerInterface> handler;
    if (listener.configuration.protocol_type == ProtocolType::RawBytes) {
        handler = std::make_unique<RawBytesProtocolHandler>(populated_id, *socket, *target_thread, listener.configuration.raw_buffer_capacity);
    } else if (listener.configuration.protocol_type == ProtocolType::TlsRawBytes) {
        handler = std::make_unique<TlsRawBytesProtocolHandler>(populated_id, *socket, *target_thread,
                                                               listener.configuration.raw_buffer_capacity,
                                                               *listener.tls_context, /*is_server=*/true);
    } else {
        handler = std::make_unique<PduProtocolHandler>(*socket, *target_thread, inbound_allocator_, logger_, populated_id);
    }
    auto conn = std::make_unique<InboundConnection>(populated_id, std::move(socket), listener.configuration.target_thread_id, std::move(handler), peer_desc,
                                                    listener.configuration.idle_timeout);
    InboundConnection* conn_ptr = conn.get();
    connections_[populated_id] = std::move(conn);
    connections_by_fd_[fd] = conn_ptr;

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLERR;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
                   "InboundConnectionManager::on_accept: epoll_ctl ADD failed for "
                   "connection from {} -- {}",
                   peer_desc, StringUtils::get_errno_string());
        teardown_connection(populated_id, "epoll_ctl failed", DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
        return;
    }

    PUBSUB_LOG(logger_, FwLogLevel::Debug, "InboundConnectionManager::on_accept: accepted connection {} from {} on port {} service [{}]",
               populated_id.get_value(), peer_desc, listener.configuration.address.port, populated_id.service_name());

    target_thread->enqueue(EventMessage::create_connection_established_event(populated_id));
}

void InboundConnectionManager::on_data_ready(InboundConnection& conn) {
    const ConnectionID id = conn.id();
    const std::string peer_desc = conn.peer_description();

    auto [ok, error, pause_reads] = conn.handle_read();
    if (!ok) {
        const std::string reason =
            error.empty() ? fmt::format("peer '{}' closed connection", peer_desc) : fmt::format("protocol error on connection from '{}': {}", peer_desc, error);

        if (error.empty()) {
            PUBSUB_LOG(logger_, FwLogLevel::Info, "InboundConnectionManager::on_data_ready: {}", reason);
        } else {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::on_data_ready: {}", reason);
        }

        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        return;
    }

    if (pause_reads) {
        // Read-side backpressure: deregister EPOLLIN on this socket. The
        // kernel TCP receive window will then close as the kernel buffer
        // fills, propagating backpressure to the peer. We preserve EPOLLOUT
        // if a send is currently in flight so writability events still come
        // through, and always keep EPOLLERR for error detection.
        const int fd = conn.get_fd();
        epoll_event ev{};
        ev.events = EPOLLERR;
        if (conn.handler()->has_pending_send()) {
            ev.events |= EPOLLOUT;
        }
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                       "InboundConnectionManager::on_data_ready: epoll_ctl MOD (pause) failed "
                       "for connection from '{}': {}",
                       peer_desc, StringUtils::get_errno_string());
        } else {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "InboundConnectionManager::on_data_ready: read backpressure engaged on "
                       "connection {} from '{}' -- EPOLLIN deregistered", id.get_value(), peer_desc);
        }
    }
}

void InboundConnectionManager::on_write_ready(InboundConnection& conn) {
    const ConnectionID id = conn.id();

    auto [ok, error] = conn.handler()->continue_send();
    if (!ok) {
        const std::string reason = error.empty()
            ? fmt::format("peer '{}' closed connection", conn.peer_description())
            : fmt::format("send error on connection from '{}': {}", conn.peer_description(), error);
        const FwLogLevel log_level = error.empty() ? FwLogLevel::Info : FwLogLevel::Error;
        PUBSUB_LOG(logger_, log_level, "InboundConnectionManager::on_write_ready: {}", reason);
        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        return;
    }

    if (!conn.handler()->has_pending_send()) {
        const int fd = conn.get_fd();
        epoll_event ev{};
        // Re-add EPOLLIN only if reads are not currently paused for
        // backpressure. The pause state is restored once the buffer drains
        // below the low-water mark via process_commit_raw_bytes().
        ev.events = EPOLLERR;
        if (!conn.handler()->is_reads_paused()) {
            ev.events |= EPOLLIN;
        }
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    }
}

void InboundConnectionManager::teardown_connection(ConnectionID id, const std::string& reason, DeliverLostEventFlag deliver_lost_event) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }

    InboundConnection& conn = *it->second;
    const ThreadID target_thread_id = conn.target_thread_id();

    PUBSUB_LOG(logger_, FwLogLevel::Info, "InboundConnectionManager::teardown_connection: connection {} from '{}': {}", id.get_value(), conn.peer_description(),
               reason);

    // Free any in-flight outbound slab chunk via the handler.
    if (conn.handler()->has_pending_send()) {
        conn.handler()->deallocate_pending_send();
    }

    // Clear pending_send_ if it refers to this connection.
    if (pending_send_.has_value() && pending_send_->connection_id_ == id) {
        pending_send_->allocator_->deallocate(pending_send_->slab_id_, pending_send_->pdu_chunk_ptr_);
        pending_send_.reset();
    }

    // Deregister from epoll and fd map.
    const int fd = conn.get_fd();
    if (fd != -1) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_by_fd_.erase(fd);
    }

    // Deliver ConnectionLost if requested.
    if (deliver_lost_event == DeliverLostEventFlag::DeliverLostEvent) {
        auto* thread = thread_lookup_.get_fast_path_thread(target_thread_id);
        if (thread != nullptr) {
            thread->enqueue(EventMessage::create_connection_lost_event(id, reason));
        }
    }

    connections_.erase(it);
}

void InboundConnectionManager::check_for_inactive_connections() {
    const auto now = std::chrono::steady_clock::now();

    // Phase 1: identify connections that have exceeded the idle timeout.
    // Collect into a vector first to avoid iterator invalidation when
    // teardown_connection() modifies connections_.
    std::vector<ConnectionID> idle_connections;
    for (const auto& [id, conn] : connections_) {
        if (conn->idle_timeout_exempt()) {
            continue;
        }
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
        const std::string reason = fmt::format("connection from '{}' closed: no data received for {}s", it->second->peer_description(),
                                               std::chrono::duration_cast<std::chrono::seconds>(config_.socket_maximum_inactivity_interval_).count());

        PUBSUB_LOG(logger_, FwLogLevel::Info, "InboundConnectionManager::check_for_inactive_connections: {}", reason);

        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
    }
}

bool InboundConnectionManager::process_send_pdu_command(const ReactorControlCommand& command) {
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

    const uint32_t total_bytes = static_cast<uint32_t>(sizeof(PduHeader)) + command.pdu_byte_count_;

    // The handler takes ownership of the slab bookkeeping from this point.
    // It stores allocator, slab_id, and chunk_ptr internally and will
    // deallocate on completion or teardown.
    auto [ok, error] = conn.handler()->send_prebuilt(command.allocator_, command.slab_id_, command.pdu_chunk_ptr_, total_bytes);
    if (!ok) {
        const std::string reason = fmt::format("send error on connection from '{}': {}", conn.peer_description(), error);
        PUBSUB_LOG(logger_, FwLogLevel::Error, "InboundConnectionManager::process_send_pdu_command: {}", reason);
        teardown_connection(cid, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        return true;
    }

    if (conn.handler()->has_pending_send()) {
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        // Add EPOLLIN only if reads are not currently paused for backpressure.
        ev.events = EPOLLOUT | EPOLLERR;
        if (!conn.handler()->is_reads_paused()) {
            ev.events |= EPOLLIN;
        }
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    }

    return true;
}

bool InboundConnectionManager::process_send_raw_command(const ReactorControlCommand& command) {
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

    auto [ok, error] = conn.handler()->send_prebuilt(command.allocator_, command.slab_id_, command.raw_chunk_ptr_, command.raw_byte_count_);
    if (!ok) {
        const std::string reason = error.empty()
            ? fmt::format("peer '{}' closed connection", conn.peer_description())
            : fmt::format("send error on connection from '{}': {}", conn.peer_description(), error);
        const FwLogLevel log_level = error.empty() ? FwLogLevel::Info : FwLogLevel::Error;
        PUBSUB_LOG(logger_, log_level, "InboundConnectionManager::process_send_raw_command: {}", reason);
        teardown_connection(cid, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        return true;
    }

    if (conn.handler()->has_pending_send()) {
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        // Add EPOLLIN only if reads are not currently paused for backpressure.
        ev.events = EPOLLOUT | EPOLLERR;
        if (!conn.handler()->is_reads_paused()) {
            ev.events |= EPOLLIN;
        }
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    }

    return true;
}

bool InboundConnectionManager::process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return false;
    }
    const InboundConnection& conn = *it->second;
    const bool resume_reads = conn.handler()->commit_bytes(bytes_consumed);

    if (resume_reads) {
        // Read-side backpressure released: re-register EPOLLIN. Preserve
        // EPOLLOUT if a send is currently in flight, and always keep EPOLLERR.
        const int fd = conn.get_fd();
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLERR;
        if (conn.handler()->has_pending_send()) {
            ev.events |= EPOLLOUT;
        }
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error,
                       "InboundConnectionManager::process_commit_raw_bytes: epoll_ctl MOD "
                       "(resume) failed for connection {} from '{}': {}",
                       id.get_value(), conn.peer_description(), StringUtils::get_errno_string());
        } else {
            PUBSUB_LOG(logger_, FwLogLevel::Info,
                       "InboundConnectionManager::process_commit_raw_bytes: read backpressure "
                       "released on connection {} from '{}' -- EPOLLIN re-registered",
                       id.get_value(), conn.peer_description());
        }
    }
    return true;
}

bool InboundConnectionManager::drain_pending_send() {
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

bool InboundConnectionManager::process_disconnect_command(ConnectionID id) {
    if (connections_.count(id) == 0) {
        return false;
    }
    teardown_connection(id, "disconnect requested by application thread", DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
    return true;
}

InboundConnection* InboundConnectionManager::find_by_fd(int fd) const {
    auto it = connections_by_fd_.find(fd);
    return (it != connections_by_fd_.end()) ? it->second : nullptr;
}

InboundConnection* InboundConnectionManager::find_by_id(ConnectionID id) const {
    auto it = connections_.find(id);
    return (it != connections_.end()) ? it->second.get() : nullptr;
}

InboundListener* InboundConnectionManager::find_listener_by_fd(int fd) {
    auto it = inbound_listeners_.find(fd);
    return (it != inbound_listeners_.end()) ? &it->second : nullptr;
}

uint16_t InboundConnectionManager::get_listener_port(int index) const {
    if (index < 0 || index >= static_cast<int>(listener_fds_in_registration_order_.size())) {
        throw PreconditionAssertion("InboundConnectionManager::get_listener_port: index out of range", __FILE__, __LINE__);
    }
    const int listen_fd = listener_fds_in_registration_order_[static_cast<size_t>(index)];
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
