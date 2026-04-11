#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/InboundConnection.hpp>
#include <pubsub_itc_fw/InboundListener.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfig.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLookupInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Manages all inbound TCP connections on behalf of the Reactor.
 *
 * @ingroup reactor_subsystem
 *
 * This class owns the inbound listener registry, the accepted connection maps,
 * and all logic for accepting, reading, writing, timing out, and tearing down
 * inbound connections. It is extracted from the Reactor to keep that class
 * focused on orchestration rather than protocol detail.
 *
 * The Reactor constructs this manager and delegates inbound epoll events to it.
 * The manager calls back into the Reactor only via ThreadLookupInterface to
 * deliver EventMessages to ApplicationThreads.
 *
 * ConnectionID allocation:
 *   ConnectionIDs are allocated by the Reactor (which maintains the shared
 *   monotonic counter used by both inbound and outbound connections) and passed
 *   into on_accept() as a parameter. This keeps the ID space unified without
 *   coupling this manager to the Reactor.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   Does not own the epoll file descriptor, ReactorConfiguration, QuillLogger,
 *   ExpandableSlabAllocator, or ThreadLookupInterface. The Reactor is responsible
 *   for their lifetimes.
 */
class InboundConnectionManager {
public:
    ~InboundConnectionManager() = default;

    InboundConnectionManager(const InboundConnectionManager&) = delete;
    InboundConnectionManager& operator=(const InboundConnectionManager&) = delete;

    /**
     * @brief Constructs an InboundConnectionManager.
     *
     * @param[in] epoll_fd          The reactor's epoll file descriptor.
     * @param[in] config            Reactor configuration (idle timeout etc.).
     *                              Must outlive this object.
     * @param[in] inbound_allocator Slab allocator for inbound PDU payloads.
     *                              Must outlive this object.
     * @param[in] thread_lookup     Interface for delivering events to threads.
     *                              Must outlive this object.
     * @param[in] logger            Logger instance. Must outlive this object.
     */
    InboundConnectionManager(int epoll_fd,
                              const ReactorConfiguration& config,
                              ExpandableSlabAllocator& inbound_allocator,
                              ThreadLookupInterface& thread_lookup,
                              QuillLogger& logger);

    /**
     * @brief Stages an inbound listener for initialisation.
     *
     * Must be called before initialize_listeners(). The listener is bound
     * and registered with epoll during initialize_listeners().
     *
     * @param[in] address          The address and port to listen on.
     * @param[in] target_thread_id The ThreadID to receive connection events.
     */
    void register_inbound_listener(NetworkEndpointConfig address, ThreadID target_thread_id);

    /**
     * @brief Binds, listens, and registers all staged listeners with epoll.
     *
     * Called once during Reactor initialisation. Populates inbound_listeners_
     * from inbound_listeners_staging_.
     *
     * @return true on success, false if any listener fails to bind or listen.
     */
    [[nodiscard]] bool initialize_listeners();

    /**
     * @brief Called when EPOLLIN fires on a listening socket.
     *
     * Accepts the connection, constructs a PduProtocolHandler and
     * InboundConnection, registers with epoll, and delivers ConnectionEstablished.
     * The ConnectionID must be pre-allocated by the Reactor and passed in here
     * to keep the shared ID space unified.
     *
     * @param[in] listener The InboundListener whose socket became readable.
     * @param[in] id       ConnectionID pre-allocated by the Reactor.
     */
    void on_accept(InboundListener& listener, ConnectionID id);

    /**
     * @brief Called when epoll signals EPOLLIN on an established inbound connection.
     *
     * Delegates to InboundConnection::handle_read(). The connection may be
     * destroyed synchronously if the disconnect handler fires during the call.
     *
     * @param[in] conn The inbound connection to service.
     */
    void on_data_ready(InboundConnection& conn);

    /**
     * @brief Called when epoll signals EPOLLOUT on an inbound connection with
     *        a partial send in flight.
     *
     * Delegates to the connection's handler. Clears EPOLLOUT when the send
     * completes. The connection may be destroyed synchronously on send error.
     *
     * @param[in] conn The inbound connection to service.
     */
    void on_write_ready(InboundConnection& conn);

    /**
     * @brief Tears down an inbound connection.
     *
     * Deregisters from epoll, frees any in-flight slab chunk, clears the
     * listener's current connection, and optionally delivers ConnectionLost.
     *
     * @param[in] id                 ConnectionID of the connection to tear down.
     * @param[in] reason             Human-readable reason for logging and event delivery.
     * @param[in] deliver_lost_event If true, delivers ConnectionLost to the target thread.
     */
    void teardown_connection(ConnectionID id, const std::string& reason,
                             bool deliver_lost_event);

    /**
     * @brief Checks all inbound connections for idle timeout and tears down
     *        any that have exceeded socket_maximum_inactivity_interval_.
     *
     * Uses the two-phase identify-then-process pattern to avoid iterator
     * invalidation during map modification.
     */
    void check_for_inactive_connections();

    /**
     * @brief Attempts to dispatch a SendPdu command to an inbound connection.
     *
     * @param[in] command The SendPdu command to process.
     * @return true if the ConnectionID belongs to an inbound connection
     *         (command was processed or stashed), false if not found here.
     */
    [[nodiscard]] bool process_send_pdu_command(const ReactorControlCommand& command);

    /**
     * @brief Drains the pending_send_ slot if one is waiting.
     *
     * Called by the Reactor at the start of process_control_commands() before
     * draining the command queue.
     *
     * @return true if pending_send_ was empty or successfully processed,
     *         false if it is still blocked.
     */
    [[nodiscard]] bool drain_pending_send();

    /**
     * @brief Attempts to tear down an inbound connection by application request.
     *
     * @param[in] id The ConnectionID to disconnect.
     * @return true if the connection was found and torn down, false otherwise.
     */
    [[nodiscard]] bool process_disconnect_command(ConnectionID id);

    /**
     * @brief Returns true if the given ConnectionID belongs to an inbound connection.
     */
    [[nodiscard]] bool owns_connection(ConnectionID id) const;

    /**
     * @brief Returns a non-owning pointer to an inbound connection by fd,
     *        or nullptr if not found. Used by the Reactor's epoll dispatch.
     */
    [[nodiscard]] InboundConnection* find_by_fd(int fd) const;

    /**
     * @brief Returns a non-owning pointer to an InboundListener by fd,
     *        or nullptr if not found. Used by the Reactor's epoll dispatch.
     */
    [[nodiscard]] InboundListener* find_listener_by_fd(int fd);

    /**
     * @brief Returns the port number assigned to the first registered listener.
     *
     * TEST SEAM. Valid only after initialize_listeners() has been called.
     * Returns 0 if no listeners are registered or the port cannot be determined.
     */
    [[nodiscard]] uint16_t get_first_listener_port() const;

private:
    int epoll_fd_;
    const ReactorConfiguration& config_;
    ExpandableSlabAllocator& inbound_allocator_;
    ThreadLookupInterface& thread_lookup_;
    QuillLogger& logger_;

    std::vector<InboundListener>                                         inbound_listeners_staging_;
    std::map<int, InboundListener>                                       inbound_listeners_;
    std::unordered_map<ConnectionID, std::unique_ptr<InboundConnection>> connections_;
    std::unordered_map<int, InboundConnection*>                          connections_by_fd_;

    std::optional<ReactorControlCommand> pending_send_;
};

} // namespace pubsub_itc_fw
