#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/DeliverLostEventFlag.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/IdleTimeoutFlag.hpp>
#include <pubsub_itc_fw/InboundConnection.hpp>
#include <pubsub_itc_fw/InboundListener.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadLookupInterface.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

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
    InboundConnectionManager(int epoll_fd, const ReactorConfiguration& config, ExpandableSlabAllocator& inbound_allocator, ThreadLookupInterface& thread_lookup,
                             QuillLogger& logger);

    /**
     * @brief Stages an inbound listener for initialisation.
     *
     * Must be called before initialize_listeners(). The listener is bound
     * and registered with epoll during initialize_listeners().
     *
     * @param[in] address             The address and port to listen on.
     * @param[in] target_thread_id    The ThreadID to receive connection events.
     * @param[in] protocol_type       Whether accepted connections use PduProtocolHandler
     *                                or RawBytesProtocolHandler. Defaults to FrameworkPdu.
     * @param[in] raw_buffer_capacity Minimum MirroredBuffer capacity in bytes for RawBytes
     *                                listeners. Ignored for FrameworkPdu listeners.
     * @param[in] idle_timeout        IdleTimeoutFlag::UseIdleTimeout (default) or
     *                                IdleTimeoutFlag::BypassIdleTimeout for listeners that
     *                                serve long-lived connections without heartbeats.
     */
    void register_inbound_listener(NetworkEndpointConfiguration address, ThreadID target_thread_id,
                                   ProtocolType protocol_type = ProtocolType{ProtocolType::FrameworkPdu}, int64_t raw_buffer_capacity = 0,
                                   IdleTimeoutFlag idle_timeout = IdleTimeoutFlag{IdleTimeoutFlag::UseIdleTimeout});

    /**
     * @brief Stages a TLS inbound listener for initialisation.
     *
     * Equivalent to register_inbound_listener with ProtocolType::TlsRawBytes, but
     * also carries the TlsListenerConfiguration needed to load certificates during
     * initialize_listeners(). Must be called before initialize_listeners().
     *
     * @param[in] address             The address and port to listen on.
     * @param[in] target_thread_id    The ThreadID to receive connection events.
     * @param[in] raw_buffer_capacity Minimum MirroredBuffer capacity in bytes.
     * @param[in] tls_config          Certificate and key paths for the TLS context.
     */
    void register_inbound_tls_listener(NetworkEndpointConfiguration address, ThreadID target_thread_id, int64_t raw_buffer_capacity,
                                       TlsListenerConfiguration tls_config);

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
    void teardown_connection(ConnectionID id, const std::string& reason, DeliverLostEventFlag deliver_lost_event);

    /**
     * @brief Checks all inbound connections for idle timeout and tears down
     *        any that have exceeded socket_maximum_inactivity_interval_.
     *
     * Uses the two-phase identify-then-process pattern to avoid iterator
     * invalidation during map modification.
     */
    void check_for_inactive_connections();

    /**
     * @brief Sends a framed message on an inbound connection, or queues it behind the sends the
     * connection is still writing.
     *
     * A connection whose queue of waiting sends is full is closed, and the application is told the
     * connection was lost: its peer is not reading what is sent to it. See WaitingSendQueue.
     *
     * @param[in] command The SendPdu command to process.
     * @return What became of the send. NoSuchConnection if no inbound connection has the id.
     */
    [[nodiscard]] SendDisposition process_send_pdu_command(const ReactorControlCommand& command);

    /**
     * @brief Attempts to satisfy a RequestWritableNotification for an inbound connection.
     *
     * If the ConnectionID belongs to an inbound connection, enqueues a
     * ConnectionWritable event to its owning ApplicationThread once the connection
     * has nothing left to write: at once if it is idle, and otherwise when the send
     * in progress and every send waiting behind it have been written. An application
     * that sends one message and then asks to be told before sending the next is so
     * paced by its peer, and builds no queue of waiting sends.
     *
     * @param[in] command The RequestWritableNotification command.
     * @return true if the ConnectionID belongs to an inbound connection, false if not.
     */
    [[nodiscard]] bool process_writable_notification_command(const ReactorControlCommand& command);

    /**
     * @brief Sends raw bytes on an inbound connection, or queues them behind the sends the connection
     * is still writing, exactly as process_send_pdu_command() does for a framed message.
     *
     * @param[in] command The SendRaw command to process.
     * @return What became of the send. NoSuchConnection if no inbound connection has the id.
     */
    [[nodiscard]] SendDisposition process_send_raw_command(const ReactorControlCommand& command);

    /**
     * @brief Stop watching a connection for incoming data, at the application's request.
     * @param[in] id The connection.
     * @return false if this manager holds no such connection.
     */
    [[nodiscard]] bool pause_reading(ConnectionID id);

    /**
     * @brief Watch a connection for incoming data again after pause_reading().
     *
     * Epoll here is level-triggered, so data that arrived while reading was paused is reported at
     * once and read.
     *
     * @param[in] id The connection.
     * @return false if this manager holds no such connection.
     */
    [[nodiscard]] bool resume_reading(ConnectionID id);

    /**
     * @brief Advances the inbound MirroredBuffer tail for a RawBytesProtocolHandler
     *        connection by the number of bytes the application has consumed.
     *
     * Called by the Reactor in response to a CommitRawBytes command. Looks up
     * the connection by ID and forwards the call to
     * ProtocolHandlerInterface::commit_bytes(). For PduProtocolHandler connections
     * this is a no-op. If the connection ID is not found, returns false so the
     * Reactor can try the outbound manager.
     *
     * @param[in] id             The ConnectionID of the raw-bytes connection.
     * @param[in] bytes_consumed Number of bytes the application has finished processing.
     * @return true if the ConnectionID belongs to an inbound connection, false otherwise.
     */
    [[nodiscard]] bool process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed);

    /**
     * @brief Attempts to tear down an inbound connection by application request.
     *
     * @param[in] id The ConnectionID to disconnect.
     * @return true if the connection was found and torn down, false otherwise.
     */
    [[nodiscard]] bool process_disconnect_command(ConnectionID id);

    /**
     * @brief Returns a non-owning pointer to an inbound connection by fd,
     *        or nullptr if not found. Used by the Reactor's epoll dispatch.
     */
    [[nodiscard]] InboundConnection* find_by_fd(int fd) const;

    /**
     * @brief Returns a non-owning pointer to an inbound connection by ConnectionID,
     *        or nullptr if not found. Used by the Reactor when processing
     *        InstallInlinePduHandler commands.
     */
    [[nodiscard]] InboundConnection* find_by_id(ConnectionID id) const;

    /**
     * @brief Returns a non-owning pointer to an InboundListener by fd,
     *        or nullptr if not found. Used by the Reactor's epoll dispatch.
     */
    [[nodiscard]] InboundListener* find_listener_by_fd(int fd);

    /**
     * @brief Returns the OS-assigned port number for the inbound listener
     *        registered at the given zero-based index.
     *
     * Listeners are indexed in the order they were registered via
     * register_inbound_listener(). Index 0 is the first registered listener,
     * index 1 the second, and so on.
     *
     * TEST SEAM. Valid only after initialize_listeners() has been called.
     *
     * @param[in] index Zero-based registration index.
     * @return The port number, or 0 if the port cannot be determined.
     * @pre index must be in range. Violating this throws PreconditionAssertion.
     */
    [[nodiscard]] uint16_t get_listener_port(int index) const;

  private:
    // Whether a connection should be watched for incoming data: neither its handler nor the application has paused it.
    [[nodiscard]] static bool wants_reads(const InboundConnection& conn);
    // Re-register a connection with epoll for what it now wants: incoming data, a send in flight, errors.
    void rearm(InboundConnection& conn);

    // Starts the send at once if the connection is writing nothing else, and otherwise adds it to the
    // connection's queue, closing the connection if the queue is full.
    SendDisposition send_or_wait(InboundConnection& conn, const WaitingSend& send);
    // Hands a send to the connection's protocol handler. Returns false if that failed and the
    // connection was closed, in which case conn no longer exists.
    [[nodiscard]] bool start_send(InboundConnection& conn, const WaitingSend& send);
    // Starts the waiting sends, oldest first, until one cannot be written at once or none is left.
    // Returns false if the connection was closed, in which case conn no longer exists.
    [[nodiscard]] bool start_waiting_sends(InboundConnection& conn);
    // Tells the application the connection can take another send, if it asked to be told and the
    // connection now has nothing left to write.
    void deliver_owed_writable_notification(InboundConnection& conn);

    int epoll_fd_;
    const ReactorConfiguration& config_;
    ExpandableSlabAllocator& inbound_allocator_;
    ThreadLookupInterface& thread_lookup_;
    QuillLogger& logger_;

    std::vector<InboundListener> inbound_listeners_staging_;
    std::map<int, InboundListener> inbound_listeners_;
    // listener_fds_in_registration_order_ records each successfully bound
    // listener's fd in the order register_inbound_listener() was called.
    // This is needed because inbound_listeners_ is keyed by fd and iterates
    // in fd order, which is not necessarily the same as registration order.
    std::vector<int> listener_fds_in_registration_order_;
    std::unordered_map<ConnectionID, std::unique_ptr<InboundConnection>> connections_;
    std::unordered_map<int, InboundConnection*> connections_by_fd_;
};

} // namespaces
