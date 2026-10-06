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
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw {

OutboundConnectionManager::OutboundConnectionManager(int epoll_fd, const ReactorConfiguration& config, ExpandableSlabAllocator& inbound_allocator,
                                                     const ServiceRegistry& service_registry, ThreadLookupInterface& thread_lookup, QuillLogger& logger)
    : epoll_fd_(epoll_fd)
    , config_(config)
    , inbound_allocator_(inbound_allocator)
    , service_registry_(service_registry)
    , thread_lookup_(thread_lookup)
    , logger_(logger) {}

void OutboundConnectionManager::process_connect_command(const ReactorControlCommand& command, ConnectionID id) {
    // connect_to_service resolved and validated the service id (fail-fast on an
    // unknown name), so mapping the id back to its name and endpoints is always
    // valid here.
    const std::string& service_name = service_registry_.service_name(command.service_id_);
    const ServiceEndpoints endpoints = service_registry_.endpoints(command.service_id_);

    const NetworkEndpointConfiguration& primary = endpoints.primary;

    auto [addr, addr_error] = InetAddress::create(primary.host, primary.port);
    if (!addr) {
        schedule_retry(service_name, command.requesting_thread_id_);
        return;
    }

    auto connector = std::make_unique<TcpConnector>();
    auto [connected_immediately, connect_error] = connector->connect(*addr);

    if (!connect_error.empty()) {
        schedule_retry(service_name, command.requesting_thread_id_);
        return;
    }

    const int fd = connector->get_fd();

    auto* target_thread = thread_lookup_.get_fast_path_thread(command.requesting_thread_id_);
    if (target_thread == nullptr) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Error, "OutboundConnectionManager::process_connect_command: requesting thread not found");
        return;
    }

    auto conn = std::make_unique<OutboundConnection>(id, command.requesting_thread_id_, service_name, endpoints, std::move(connector), inbound_allocator_,
                                                     *target_thread, logger_);

    OutboundConnection* conn_ptr = conn.get();
    connections_[id] = std::move(conn);
    connections_by_fd_[fd] = conn_ptr;

    if (connected_immediately) {
        on_connect_ready(*conn_ptr);
    } else {
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLERR;
        ev.data.fd = fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::process_connect_command: epoll_ctl ADD failed for fd {}", fd);
            teardown_connection(id, "epoll_ctl failed during connect", DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
            target_thread->enqueue(EventMessage::create_connection_failed_event("epoll_ctl failed during connect"));
        }
    }
}

void OutboundConnectionManager::on_connect_ready(OutboundConnection& conn) {
    auto [connected, error] = conn.connector()->finish_connect();

    if (!connected) {
        if (!error.empty()) {
            if (retry_contexts_.find(conn.service_name()) == retry_contexts_.end()) {
                const bool runtime_failure = ever_established_services_.count(conn.service_name()) > 0;
                PUBSUB_LOG(logger_, runtime_failure ? FwLogLevel::Warning : FwLogLevel::Info,
                           "OutboundConnectionManager::on_connect_ready: finish_connect failed for "
                           "service '{}': {}",
                           conn.service_name(), error);
            }

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
                               "secondary {}:{} -- {}",
                               secondary.host, secondary.port, addr_error);
                    teardown_connection(conn.id(), addr_error, DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
                    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
                    if (thread != nullptr) {
                        thread->enqueue(EventMessage::create_connection_failed_event(addr_error));
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
                    teardown_connection(conn.id(), connect_error, DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
                    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
                    if (thread != nullptr) {
                        thread->enqueue(EventMessage::create_connection_failed_event(connect_error));
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
                    ev.events = EPOLLOUT | EPOLLERR;
                    ev.data.fd = new_fd;
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, new_fd, &ev);
                }
                return;
            }

            // Save the service name and requesting thread before teardown
            // destroys conn -- accessing conn after teardown_connection is
            // use-after-free since teardown erases conn from the connections_ map.
            const std::string service_name = conn.service_name();
            const ThreadID requesting_thread_id = conn.requesting_thread_id();

            teardown_connection(conn.id(), error, DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
            schedule_retry(service_name, requesting_thread_id);
        }
        // else still in progress -- wait for next EPOLLOUT
        return;
    }

    const int fd = conn.get_fd();

    auto socket = conn.connector()->get_connected_socket();
    conn.on_connected(std::move(socket));

    if (config_.socket_send_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &config_.socket_send_buffer_size, sizeof(config_.socket_send_buffer_size));
    }
    if (config_.socket_receive_buffer_size > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config_.socket_receive_buffer_size, sizeof(config_.socket_receive_buffer_size));
    }

    if (conn.is_tls()) {
        auto [ok, handshake_error] = conn.protocol_handler()->start_outbound_handshake();
        if (!ok) {
            const std::string service_name = conn.service_name();
            const ThreadID requesting_thread_id = conn.requesting_thread_id();
            const ConnectionID conn_id = conn.id();
            PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::on_connect_ready: TLS handshake initiation failed for service '{}': {}",
                       service_name, handshake_error);
            teardown_connection(conn_id, handshake_error, DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
            schedule_retry(service_name, requesting_thread_id);
            return;
        }

        uint32_t epoll_events = EPOLLIN | EPOLLERR;
        if (conn.protocol_handler()->has_pending_send()) {
            epoll_events |= EPOLLOUT;
        }
        epoll_event ev{};
        ev.events = epoll_events;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);

        PUBSUB_LOG(logger_, FwLogLevel::Info, "OutboundConnectionManager::on_connect_ready: TLS handshake initiated for service '{}'", conn.service_name());
        // ConnectionEstablished is delivered from on_data_ready() once the handshake completes.
    } else {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLERR;
        ev.data.fd = fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);

        {
            auto ctx_it = retry_contexts_.find(conn.service_name());
            if (ctx_it != retry_contexts_.end()) {
                const auto elapsed_s =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - ctx_it->second.first_fail_time).count();
                const bool runtime_reconnect = ever_established_services_.count(conn.service_name()) > 0;
                PUBSUB_LOG(logger_, runtime_reconnect ? FwLogLevel::Warning : FwLogLevel::Info, "OutboundConnectionManager: service '{}' reconnected after {}s",
                           conn.service_name(), elapsed_s);
                retry_contexts_.erase(ctx_it);
            } else {
                PUBSUB_LOG(logger_, FwLogLevel::Info, "OutboundConnectionManager::on_connect_ready: connection {} to service '{}' established",
                           conn.id().get_value(), conn.service_name());
            }
            ever_established_services_.insert(conn.service_name());
        }

        auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
        if (thread != nullptr) {
            thread->enqueue(EventMessage::create_connection_established_event(ConnectionID{conn.id().get_value(), conn.service_name()}));
        }

        // Sends asked for before the connection was established waited for it, and go now, in order.
        if (start_waiting_sends(conn)) {
            deliver_owed_writable_notification(conn);
        }
    }
}

void OutboundConnectionManager::on_data_ready(OutboundConnection& conn) {
    const ConnectionID id = conn.id();
    const std::string service_name = conn.service_name();
    const ThreadID requesting_thread_id = conn.requesting_thread_id();

    if (conn.is_tls()) {
        const bool was_established = conn.is_established();

        auto [ok, error, pause_reads] = conn.protocol_handler()->on_data_ready();
        if (!ok) {
            const std::string reason = error.empty() ? fmt::format("peer closed TLS connection on service '{}'", service_name)
                                                     : fmt::format("TLS error on service '{}': {}", service_name, error);
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "OutboundConnectionManager::on_data_ready: {}", reason);
            teardown_connection(id, reason,
                                was_established ? DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent}
                                                : DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
            schedule_retry(service_name, requesting_thread_id);
            return;
        }

        if (!was_established && conn.protocol_handler()->is_handshake_complete()) {
            conn.mark_as_established();
            {
                auto ctx_it = retry_contexts_.find(service_name);
                if (ctx_it != retry_contexts_.end()) {
                    const auto elapsed_s =
                        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - ctx_it->second.first_fail_time).count();
                    const bool runtime_reconnect = ever_established_services_.count(service_name) > 0;
                    PUBSUB_LOG(logger_, runtime_reconnect ? FwLogLevel::Warning : FwLogLevel::Info,
                               "OutboundConnectionManager: service '{}' reconnected (TLS) after {}s", service_name, elapsed_s);
                    retry_contexts_.erase(ctx_it);
                } else {
                    PUBSUB_LOG(logger_, FwLogLevel::Info,
                               "OutboundConnectionManager::on_data_ready: TLS handshake complete, connection {} to service '{}' established", id.get_value(),
                               service_name);
                }
                ever_established_services_.insert(service_name);
            }
            auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
            if (thread != nullptr) {
                thread->enqueue(EventMessage::create_connection_established_event(ConnectionID{id.get_value(), service_name}));
            }

            // Sends asked for during the handshake waited for it, and go now, in order.
            if (!start_waiting_sends(conn)) {
                return;
            }
            deliver_owed_writable_notification(conn);
        }

        const bool needs_epoll_mod = pause_reads || conn.protocol_handler()->has_pending_send();
        if (needs_epoll_mod) {
            const int conn_fd = conn.get_fd();
            epoll_event ev{};
            ev.events = EPOLLERR;
            if (!pause_reads && !conn.reading_paused_by_application()) {
                ev.events |= EPOLLIN;
            }
            if (conn.protocol_handler()->has_pending_send()) {
                ev.events |= EPOLLOUT;
            }
            ev.data.fd = conn_fd;
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
        }
        return;
    }

    // PDU path
    auto [ok, error] = conn.parser()->receive();
    if (!ok) {
        const std::string reason = error.empty() ? fmt::format("peer closed connection on service '{}'", service_name)
                                                 : fmt::format("parse error on service '{}': {}", service_name, error);

        PUBSUB_LOG(logger_, FwLogLevel::Warning, "OutboundConnectionManager::on_data_ready: {}", reason);

        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        schedule_retry(service_name, requesting_thread_id);
    }
}

void OutboundConnectionManager::handle_socket_error(OutboundConnection& conn) {
    // Snapshot before the teardown, which erases the OutboundConnection from connections_ and so
    // takes the service name and the thread the retry has to be issued for with it.
    const std::string service_name = conn.service_name();
    const ThreadID requesting_thread_id = conn.requesting_thread_id();
    const ConnectionID id = conn.id();

    int error = 0;
    socklen_t error_length = sizeof(error);
    ::getsockopt(conn.get_fd(), SOL_SOCKET, SO_ERROR, &error, &error_length);
    const std::string reason =
        fmt::format("socket error on connection {} to service '{}': {}", id.get_value(), service_name, StringUtils::get_error_string(error));

    PUBSUB_LOG(logger_, FwLogLevel::Warning, "OutboundConnectionManager::handle_socket_error: {}", reason);
    teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
    schedule_retry(service_name, requesting_thread_id);
}

void OutboundConnectionManager::on_write_ready(OutboundConnection& conn) {
    if (conn.is_tls()) {
        auto [ok, error] = conn.protocol_handler()->continue_send();
        if (!ok) {
            const std::string service_name = conn.service_name();
            const ThreadID requesting_thread_id = conn.requesting_thread_id();
            const ConnectionID id = conn.id();
            const bool was_established = conn.is_established();
            const std::string reason = fmt::format("TLS send error on service '{}': {}", service_name, error);
            PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::on_write_ready: {}", reason);
            teardown_connection(id, reason,
                                was_established ? DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent}
                                                : DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});
            schedule_retry(service_name, requesting_thread_id);
            return;
        }

        if (conn.protocol_handler()->has_pending_send()) {
            return;
        }
        // The send in progress is written; the sends that waited behind it are started next, in order.
        // Before the handshake completes there is nothing of the application's to start.
        if (conn.is_established() && !start_waiting_sends(conn)) {
            return;
        }
        if (!conn.protocol_handler()->has_pending_send()) {
            // Stop watching for room to write. Set here rather than through rearm(), which leaves a
            // connection still in its handshake alone: a connection left watched for room to write
            // with nothing to write would wake the reactor continuously.
            const int fd = conn.get_fd();
            epoll_event ev{};
            ev.events = EPOLLERR;
            if (wants_reads(conn)) {
                ev.events |= EPOLLIN;
            }
            ev.data.fd = fd;
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
            deliver_owed_writable_notification(conn);
        }
        return;
    }

    // PDU path
    auto [ok, error] = conn.framer()->continue_send();
    if (!ok) {
        const std::string service_name = conn.service_name();
        const ThreadID requesting_thread_id = conn.requesting_thread_id();
        const ConnectionID id = conn.id();
        const std::string reason = fmt::format("send error on service '{}': {}", service_name, error);
        PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::on_write_ready: {}", reason);
        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        schedule_retry(service_name, requesting_thread_id);
        return;
    }

    if (conn.framer()->has_pending_data()) {
        return;
    }
    conn.current_allocator()->deallocate(conn.current_slab_id(), conn.current_chunk_ptr());
    conn.clear_pending_send();

    // The send in progress is written; the sends that waited behind it are started next, in order.
    if (!start_waiting_sends(conn)) {
        return;
    }
    if (!conn.has_pending_send()) {
        rearm(conn);
        deliver_owed_writable_notification(conn);
    }
}

bool OutboundConnectionManager::process_writable_notification_command(const ReactorControlCommand& command) {
    const ConnectionID cid = command.connection_id_;

    auto it = connections_.find(cid);
    if (it == connections_.end()) {
        return false;
    }

    OutboundConnection& conn = *it->second;
    conn.want_writable_notification();
    deliver_owed_writable_notification(conn);
    return true;
}

void OutboundConnectionManager::deliver_owed_writable_notification(OutboundConnection& conn) {
    // Not before the connection is established, nor while anything is left to write: the application
    // asked so as to be paced by its peer, and telling it now would let it build a queue the peer is
    // not reading.
    if (!conn.is_established() || conn.has_pending_send() || !conn.waiting_sends().empty() || !conn.take_writable_notification_wanted()) {
        return;
    }
    auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
    if (thread != nullptr) {
        thread->enqueue(EventMessage::create_connection_writable_event(ConnectionID{conn.id().get_value(), conn.service_name()}));
    }
}

SendDisposition OutboundConnectionManager::process_send_pdu_command(const ReactorControlCommand& command) {
    auto it = connections_.find(command.connection_id_);
    if (it == connections_.end()) {
        return SendDisposition::NoSuchConnection;
    }
    const WaitingSend send{command.allocator_, command.slab_id_, command.pdu_chunk_ptr_, static_cast<uint32_t>(sizeof(PduHeader)) + command.pdu_byte_count_};
    return send_or_wait(*it->second, send);
}

SendDisposition OutboundConnectionManager::process_send_raw_command(const ReactorControlCommand& command) {
    auto it = connections_.find(command.connection_id_);
    if (it == connections_.end() || !it->second->is_tls()) {
        // A plain TCP outbound connection carries framed messages, not raw bytes.
        return SendDisposition::NoSuchConnection;
    }
    const WaitingSend send{command.allocator_, command.slab_id_, command.raw_chunk_ptr_, command.raw_byte_count_};
    return send_or_wait(*it->second, send);
}

SendDisposition OutboundConnectionManager::send_or_wait(OutboundConnection& conn, const WaitingSend& send) {
    // A send waits if the connection is not yet established, if it is still writing an earlier send,
    // or if others are already waiting, so that they are written in the order asked.
    if (conn.is_established() && !conn.has_pending_send() && conn.waiting_sends().empty()) {
        static_cast<void>(start_send(conn, send));
        return SendDisposition::Started;
    }
    if (conn.waiting_sends().add(send, config_.connection_waiting_sends_maximum, config_.connection_waiting_bytes_maximum)) {
        return SendDisposition::Waiting;
    }

    // The peer is not reading what is sent to it, or the connection has not been established in all
    // the time these sends took to be asked for. It is closed rather than allowed to hold memory
    // without limit, the application is told it was lost, and a reconnection is scheduled.
    send.allocator->deallocate(send.slab_id, send.chunk);
    const std::string service_name = conn.service_name();
    const ThreadID requesting_thread_id = conn.requesting_thread_id();
    const std::string reason = fmt::format("connection {} to service '{}' closed: it is not reading what is sent to it -- {} sends of {} bytes in all "
                                           "are waiting to be written, the most allowed",
                                           conn.id().get_value(), service_name, conn.waiting_sends().size(), conn.waiting_sends().bytes());
    PUBSUB_LOG(logger_, FwLogLevel::Warning, "OutboundConnectionManager::send_or_wait: {}", reason);
    teardown_connection(conn.id(), reason, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
    schedule_retry(service_name, requesting_thread_id);
    return SendDisposition::Waiting;
}

bool OutboundConnectionManager::start_send(OutboundConnection& conn, const WaitingSend& send) {
    const std::string service_name = conn.service_name();
    const ThreadID requesting_thread_id = conn.requesting_thread_id();
    const ConnectionID conn_id = conn.id();

    if (conn.is_tls()) {
        // The TLS handler encrypts the bytes into its own buffer and returns the chunk at once.
        auto [ok, send_error] = conn.protocol_handler()->send_prebuilt(send.allocator, send.slab_id, send.chunk, send.byte_count);
        if (!ok) {
            PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::start_send: TLS send error on service '{}': {}", service_name, send_error);
            teardown_connection(conn_id, send_error, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
            schedule_retry(service_name, requesting_thread_id);
            return false;
        }
        if (conn.protocol_handler()->has_pending_send()) {
            rearm(conn);
        }
        return true;
    }

    auto [ok, send_error] = conn.framer()->send_prebuilt(static_cast<const uint8_t*>(send.chunk), send.byte_count);
    if (!ok) {
        PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::start_send: send error to '{}': {}", service_name, send_error);
        send.allocator->deallocate(send.slab_id, send.chunk);
        teardown_connection(conn_id, send_error, DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
        schedule_retry(service_name, requesting_thread_id);
        return false;
    }
    if (conn.framer()->has_pending_data()) {
        // Not all of it could be written. The connection keeps the chunk until the rest is, and is
        // watched for room to write it.
        conn.set_pending_send(send.allocator, send.slab_id, send.chunk, send.byte_count);
        rearm(conn);
    } else {
        send.allocator->deallocate(send.slab_id, send.chunk);
    }
    return true;
}

bool OutboundConnectionManager::start_waiting_sends(OutboundConnection& conn) {
    WaitingSendQueue& waiting = conn.waiting_sends();
    while (!waiting.empty() && !conn.has_pending_send()) {
        const WaitingSend next = waiting.front();
        waiting.pop_front();
        if (!start_send(conn, next)) {
            return false;
        }
    }
    return true;
}

bool OutboundConnectionManager::process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return false;
    }

    const OutboundConnection& conn = *it->second;
    if (!conn.is_tls() || conn.protocol_handler() == nullptr) {
        // PDU connections have no MirroredBuffer; this is a no-op but we own the ID.
        return true;
    }

    const bool resume_reads = conn.protocol_handler()->commit_bytes(bytes_consumed);
    if (resume_reads && !conn.reading_paused_by_application()) {
        const int conn_fd = conn.get_fd();
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLERR;
        if (conn.protocol_handler()->has_pending_send()) {
            ev.events |= EPOLLOUT;
        }
        ev.data.fd = conn_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn_fd, &ev);
    }
    return true;
}

bool OutboundConnectionManager::process_disconnect_command(ConnectionID id) {
    if (connections_.count(id) == 0) {
        return false;
    }
    teardown_connection(id, "disconnect requested by application thread", DeliverLostEventFlag{DeliverLostEventFlag::DeliverLostEvent});
    return true;
}

void OutboundConnectionManager::schedule_retry(const std::string& service_name, ThreadID requesting_thread_id) {
    ReactorControlCommand retry_cmd{ReactorControlCommand::CommandTag::Connect};
    retry_cmd.requesting_thread_id_ = requesting_thread_id;
    // service_name is an already-registered service (it came from a resolved
    // connect), so this resolves to a valid ServiceID.
    retry_cmd.service_id_ = service_registry_.resolve(service_name);
    const auto now = std::chrono::steady_clock::now();
    pending_retries_[service_name] = PendingRetry(retry_cmd, now + config_.connect_retry_interval_);

    if (retry_contexts_.find(service_name) == retry_contexts_.end()) {
        // First failure for this service -- log once and create context.
        retry_contexts_[service_name] = RetryContext{now, now};
        const bool runtime_failure = ever_established_services_.count(service_name) > 0;
        const FwLogLevel level = runtime_failure ? FwLogLevel::Warning : FwLogLevel::Info;
        const auto warning_min = std::chrono::duration_cast<std::chrono::minutes>(config_.connect_retry_warning_interval_).count();
        if (warning_min > 0) {
            PUBSUB_LOG(logger_, level,
                       "OutboundConnectionManager: service '{}' failed to connect; "
                       "retrying every {}ms (next reminder in {}min if still down)",
                       service_name, config_.connect_retry_interval_.count(), warning_min);
        } else {
            PUBSUB_LOG(logger_, level,
                       "OutboundConnectionManager: service '{}' failed to connect; "
                       "retrying every {}ms",
                       service_name, config_.connect_retry_interval_.count());
        }
    }
    // Subsequent retries are silent -- handled by periodic reminder in retry_failed_connections().
}

void OutboundConnectionManager::retry_failed_connections(const std::function<ConnectionID()>& next_id_fn) {
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
        const ReactorControlCommand cmd = it->second.command;
        pending_retries_.erase(it);

        // Emit periodic "still failing" reminder if the warning interval has elapsed.
        auto ctx_it = retry_contexts_.find(service_name);
        if (ctx_it != retry_contexts_.end() && config_.connect_retry_warning_interval_.count() > 0) {
            if (now - ctx_it->second.last_warning_time >= config_.connect_retry_warning_interval_) {
                const auto total_s = std::chrono::duration_cast<std::chrono::seconds>(now - ctx_it->second.first_fail_time).count();
                PUBSUB_LOG(logger_, FwLogLevel::Warning,
                           "OutboundConnectionManager: service '{}' still not connected "
                           "({} seconds disconnected); still retrying every {}ms",
                           service_name, total_s, config_.connect_retry_interval_.count());
                ctx_it->second.last_warning_time = now;
            }
        }

        process_connect_command(cmd, next_id_fn());
    }
}

void OutboundConnectionManager::check_for_timed_out_connections() {
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
        const std::string service_name = it->second->service_name();
        const ThreadID requesting_thread_id = it->second->requesting_thread_id();
        const std::string reason = fmt::format("connect timeout after {}ms for service '{}'",
                                               std::chrono::duration_cast<std::chrono::milliseconds>(config_.connect_timeout).count(), service_name);

        PUBSUB_LOG(logger_, FwLogLevel::Warning, "OutboundConnectionManager::check_for_timed_out_connections: {}", reason);

        teardown_connection(id, reason, DeliverLostEventFlag{DeliverLostEventFlag::SuppressLostEvent});

        auto* thread = thread_lookup_.get_fast_path_thread(requesting_thread_id);
        if (thread != nullptr) {
            thread->enqueue(EventMessage::create_connection_failed_event(reason));
        }

        schedule_retry(service_name, requesting_thread_id);
    }
}

OutboundConnection* OutboundConnectionManager::find_by_fd(int fd) const {
    auto it = connections_by_fd_.find(fd);
    return (it != connections_by_fd_.end()) ? it->second : nullptr;
}

OutboundConnection* OutboundConnectionManager::find_by_id(ConnectionID id) const {
    auto it = connections_.find(id);
    return (it != connections_.end()) ? it->second.get() : nullptr;
}

void OutboundConnectionManager::teardown_connection(ConnectionID id, const std::string& reason, DeliverLostEventFlag deliver_lost_event) {
    auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }

    OutboundConnection& conn = *it->second;

    if (retry_contexts_.find(conn.service_name()) == retry_contexts_.end()) {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "OutboundConnectionManager::teardown_connection: connection {} service '{}': {}", id.get_value(),
                   conn.service_name(), reason);
    }

    // Free any in-flight outbound data.
    if (conn.has_pending_send()) {
        if (conn.is_tls()) {
            // TLS: the slab was freed in send_prebuilt(); only the ciphertext buffer needs clearing.
            conn.protocol_handler()->deallocate_pending_send();
        } else {
            conn.current_allocator()->deallocate(conn.current_slab_id(), conn.current_chunk_ptr());
            conn.clear_pending_send();
        }
    }

    // Return the chunks of the sends still waiting for this connection; they will never be written.
    // None of them has been handed to the framer or the TLS handler yet.
    conn.waiting_sends().release_all();

    // Deregister from epoll and remove from fd map.
    const int fd = conn.get_fd();
    if (fd != -1) {
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_by_fd_.erase(fd);
    } else {
        // finish_connect() called cancel() before teardown, closing the fd and
        // making get_fd() return -1. Epoll auto-removed the closed fd, but the
        // connections_by_fd_ entry still points to this (about-to-be-freed)
        // connection. Scan to remove it before the memory is freed.
        for (auto it2 = connections_by_fd_.begin(); it2 != connections_by_fd_.end(); ++it2) {
            if (it2->second == &conn) {
                connections_by_fd_.erase(it2);
                break;
            }
        }
    }

    // Deliver ConnectionLost if requested and the connection was established.
    if (deliver_lost_event == DeliverLostEventFlag::DeliverLostEvent && conn.is_established()) {
        auto* thread = thread_lookup_.get_fast_path_thread(conn.requesting_thread_id());
        if (thread != nullptr) {
            thread->enqueue(EventMessage::create_connection_lost_event(id, reason));
        }
    }

    connections_.erase(it);
}

bool OutboundConnectionManager::wants_reads(const OutboundConnection& conn) {
    const bool paused_by_handler = conn.protocol_handler() != nullptr && conn.protocol_handler()->is_reads_paused();
    return !paused_by_handler && !conn.reading_paused_by_application();
}

void OutboundConnectionManager::rearm(OutboundConnection& conn) {
    if (!conn.is_established()) {
        // Before it is established a connection is watched for the connect completing, not for
        // data; the application's pause takes effect when it is.
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLERR;
    if (wants_reads(conn)) {
        ev.events |= EPOLLIN;
    }
    if (conn.has_pending_send()) {
        ev.events |= EPOLLOUT;
    }
    ev.data.fd = conn.get_fd();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.get_fd(), &ev) == -1) {
        PUBSUB_LOG(logger_, FwLogLevel::Error, "OutboundConnectionManager::rearm: epoll_ctl MOD failed for connection {} to '{}': {}", conn.id().get_value(),
                   conn.service_name(), StringUtils::get_errno_string());
    }
}

bool OutboundConnectionManager::pause_reading(ConnectionID id) {
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return false;
    }
    it->second->pause_reading_by_application();
    rearm(*it->second);
    return true;
}

bool OutboundConnectionManager::resume_reading(ConnectionID id) {
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return false;
    }
    it->second->resume_reading_by_application();
    rearm(*it->second);
    return true;
}

} // namespaces
